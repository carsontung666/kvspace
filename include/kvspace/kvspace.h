/*
 * kvspace.h — KVSpace C ABI（权威定义）
 *
 * 这是 kvspace-c（SHM）与 kvspace-durable（redis/fs/s3）两个后端共同实现的
 * 唯一 C ABI。消费者只链接 libkvspace（dispatch 前端），运行期按 DSN scheme 选择后端：
 *   shm://...      → libkvspace-c.so.1
 *   其余（redis/fs/s3）→ libkvspace_durable.so.1
 *
 * Wire: [len:u48 LE@0-5][storetype:u8@6][ptr:u8@7][body@8][attr@8+len].
 * Head is fixed 8 bytes; body offset is always 8. attr = [attr_len:u16 LE]
 * + records [tag:u8][len:u16 LE][data]; tag 1 = langtype (plaintext UTF-8,
 * incl. leading [dims]). Empty attr = 2 bytes. Single-key write — no attr key.
 * See kvspace/xvalue_head.h.
 */

#ifndef KVSPACE_H
#define KVSPACE_H

#include <stdint.h>

#include "kvspace/const.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 存储位置种类。 */
#define KVSPACE_REF_INLINE 0
#define KVSPACE_REF_PTR    1
#define KVSPACE_REF_EXT    2

/* Storage classes (kvspaceXvalueHead.storetype wire byte). */
#define KVSPACE_STORETYPE_FIXED_SMALL 0
#define KVSPACE_STORETYPE_SLACK       1
#define KVSPACE_STORETYPE_FIXED_LARGE 2
#define KVSPACE_STORETYPE_EXT         3

/* XValue metadata. langtype is read from attr tag=1; ndim/dims are parsed from leading [dims]. */
typedef struct {
    uint16_t headlen;       /* head 总字节数；body 起于偏移 headlen */
    uint8_t  ref;           /* 存储位置：见 KVSPACE_REF_* */
    uint8_t  storetype;     /* Storage class. */
    int32_t  body_len;      /* body 字节数 */
    int32_t  ndim;          /* Tensor dimensions; zero for other values. */
    int32_t  dims[8];       /* Tensor shape. */
    char     langtype[256]; /* 语义类型 kindexpr 串，NUL 终止（含 [dims]、无 ptr/ext 前缀） */
    int32_t  langtype_len;  /* langtype 内容长度（去 padding） */
    int32_t  body_offset;   /* body 在 data 内的起始偏移（= headlen） */
    uint64_t body_cap;
} kvspaceHead_t;

/* ── 生命周期 ─────────────────────────────────────────────────── */
void *kvspaceConnect(const char *dsn);

/* 关闭：关闭前落盘未决写（失败写 stderr，绝不静默）。
 * 进程正常退出（exit / main 返回）时，前端对所有未 Close 句柄兜底关闭即落盘——
 * 漏调 Close 不丢数据，真丢也绝不静默。 */
void  kvspaceClose(void *h);

/* ── 单点读写 / 目录 ──────────────────────────────────────────── */

/* Borrowed read: *out points into backend storage (shm mmap / durable read cache),
 * valid until the key is overwritten/deleted or the handle is closed; do not free.
 * Missing/empty key -> *out=NULL, *out_len=0, returns 0.
 * resolve != 0 follows leaf Ptr chains (up to 16 hops; loops/dangling treated as missing);
 * resolve == 0 returns the leaf wire as-is without following leaf Ptr.
 * Exception (volatile keys): `/vthread/<vid>/‥status` re-reads the backend on each call,
 * keeping the previous view alive only until the next read of that key. */
int kvspaceGet(void *h, const char *key, int resolve, uint8_t **out, uint32_t *out_len);
/* Full-value write: writes a complete XValue (validating wire header and attr bounds).
 * Stores syntactically valid Ptr XValues at the wire layer without dereferencing the target
 * or checking its current langtype (allowing multi-hop `slot -> ‥arg` bindings and forward/cyclic pointers). */
int kvspaceSetValue(void *h, const char *key, const uint8_t *value, uint32_t value_len,
                    char *err, uint32_t err_cap);

/* ResolveRef：block_id=叶子、gen=0；parent_id=目录祖先 ART 节点，
 * depth=进入该节点时 key 已消费字节数。GetByRef：gen==0 直取叶子；
 * gen>0 则 block_id 为父节点、gen 为 depth，从该处续走剩余字节。
 * 后端未实现父节点时 parent_id 保持 0。 */
typedef struct {
    uint32_t block_id;
    uint32_t gen;
    uint32_t parent_id;
    uint32_t depth;
} kvspaceRef_t;
int kvspaceResolveRef(void *h, const char *key, kvspaceRef_t *ref);
int kvspaceGetByRef(void *h, kvspaceRef_t *ref, const char *key_fallback,
                    uint8_t **out, uint32_t *out_len);
int kvspaceSetPartByRef(void *h, kvspaceRef_t *ref, const char *key_fallback,
                        uint32_t offset, const uint8_t *buf, uint32_t buf_len,
                        char *err, uint32_t err_cap);
/* 整值写 by cached ref：与 kvspaceSetValue 同语义；ref 失效时后端退回 key 的完整写
 * 并重解析。后端未实现该符号时，前端退回按 key 的 SetValue。 */
int kvspaceSetValueByRef(void *h, kvspaceRef_t *ref, const char *key_fallback,
                         const uint8_t *value, uint32_t value_len,
                         char *err, uint32_t err_cap);

/* shm-only slot view: exposes mapped slot addresses for direct per-step read/write.
 * `head` points to XValue start, `body = head + headlen`; `cap` is physical box capacity,
 * `body_cap = cap - headlen`. Borrowed until key overwrite/delete, view invalidation, or close.
 * Returns 0 on success, -1 if missing/invalid/non-viewable, -2 if unsupported by backend
 * (including when the backend does not export this symbol; caller falls back once per handle). */
typedef struct {
    uint8_t *head;
    uint32_t headlen;
    uint8_t *body;
    uint64_t body_cap;
    uint64_t cap;
} kvspaceXvalueBodyView_t;
int kvspaceXvalueBodyView(void *h, const kvspaceRef_t *ref,
                    const char *key_fallback, kvspaceXvalueBodyView_t *out);

/* ── P2: 版本化 view token（前缀/leaf 缓存载体）─────────────────── */
typedef struct {
    uint8_t *head;        /* XValue 起点（shm 映射内，借用） */
    uint8_t *body;        /* head + headlen */
    uint32_t headlen;
    uint32_t value_len;
    uint64_t cap;         /* 整 box 物理容量 */
    uint64_t body_cap;
    uint32_t struct_gen;  /* 结构版本：子集变化/搬迁/重分配才变 */
    uint32_t value_gen;   /* 值版本：每次值写/删递增（seqlock） */
    uint64_t store_id;    /* 句柄身份：close/重开后旧 token 失效 */
    uint32_t node_id;
    uint32_t _pad;
} kvspaceViewToken_t;

int kvspaceViewSweep(void *h);
int kvspaceViewAck(void *h, uint32_t *epoch);
int kvspaceViewQuiesce(void *h, int on);
int kvspaceViewResolvePrefix(void *h, const char *path, kvspaceRef_t *ref);
int kvspaceViewResolve(void *h, const char *key, kvspaceViewToken_t *tok);
int kvspaceViewFromRef(void *h, const kvspaceRef_t *ref, const char *key_fallback,
                       kvspaceViewToken_t *tok);
int kvspaceViewResolveFrom(void *h, const kvspaceRef_t *parent, const char *rel,
                           kvspaceViewToken_t *tok);
int kvspaceViewCheck(void *h, const kvspaceViewToken_t *tok);
int kvspaceViewWrite(void *h, kvspaceViewToken_t *tok, const uint8_t *data, uint32_t len);
int kvspaceViewDel(void *h, kvspaceViewToken_t *tok, const char *key);
int kvspaceViewRefresh(void *h, kvspaceViewToken_t *tok, const char *key);


/* 定位读：借用读 key 值的 [offset, offset+len) 字节，*out 指向后端常驻空间（借用，不得 free，
 * 生命周期到该 key 被写/删或 handle close；volatile 键例外，见上）。恒穿透 link。
 * 越界/空/不存在 → *out=NULL、*out_len=0、返回 0。 */
int kvspaceGetPart(void *h, const char *key, uint32_t offset, uint32_t len,
                    uint8_t **out, uint32_t *out_len);

/* 定位写：就地写 buf 到 key 值的 [offset, offset+buf_len)（key 须已存在、不改结构/尺寸）。
 * 写即持久。越界/不存在 → 非 0 + err。用于 ndarray 元素/head 的分片就地更新。 */
int kvspaceSetPart(void *h, const char *key, uint32_t offset, const uint8_t *buf,
                    uint32_t buf_len, char *err, uint32_t err_cap);

/* Read the wire head + langtype tail. Missing keys return nonzero. */
int kvspaceGetHead(void *h, const char *key, kvspaceHead_t *out);

/* In-place write: key must exist with unchanged kind and body_len; returns existing body
 * pointer for direct mutation, or nonzero + err on precondition failure (never reallocates).
 * resolve != 0 follows leaf Ptr chains (up to 16 hops) and writes the terminal target in place;
 * resolve == 0 operates on the leaf itself without following leaf Ptr.
 * Flushed on the next kvspace* call or kvspaceClose. */
int kvspaceWriteInPlace(void *h, const char *key, int resolve, uint32_t body_len,
                        uint8_t **body, char *err, uint32_t err_cap);

/* Reserve a new XValue and return its body pointer; fill it before another KVSpace call.
 * langtype (empty/NULL = untyped) is written with the header/attr in a single-key write. */
int kvspaceWriteNewPlace(void *h, const char *key, uint8_t ref, uint8_t storetype,
                         uint32_t body_len, uint64_t body_cap, const char *langtype,
                         uint8_t **body, char *err, uint32_t err_cap);

/* 只返回前缀下子项计数，无缓冲、无需释放。resolve=1 穿透 link。 */
int kvspaceListLen(void *h, const char *prefix, int expand_ext, int resolve, int32_t *out_count);

/* 索引取项：把前缀下第 idx 个直接子项名写进调用方自备缓冲 buf（容量 buf_cap），*out_len
 * 置该名长度（不含 NUL）。库侧零状态、调用方不得 free。idx 越界或缓冲不足 → 返回非 0
 * （缓冲不足时 *out_len 仍为所需长度，不静默截断）。配合 kvspaceListLen 遍历。
 * resolve=1 穿透 link；expand_ext=1 展开 extindex。 */
int kvspaceListAt(void *h, const char *prefix, int expand_ext, int resolve, int32_t idx,
                  uint8_t *buf, uint32_t buf_cap, uint32_t *out_len);

int kvspaceDel(void *h, const char *const *keys, uint32_t nkeys, char *err, uint32_t err_cap);
int kvspaceDelTree(void *h, const char *prefix, char *err, uint32_t err_cap);
int kvspaceCp(void *h, const char *src, const char *dst, char *err, uint32_t err_cap);
int kvspaceCpTree(void *h, const char *src, const char *dst, char *err, uint32_t err_cap);
int kvspaceCpList(void *h, const char *src, const char *dst, char *err, uint32_t err_cap);
int kvspaceMkindex(void *h, const char *path, uint32_t capacity, char *err, uint32_t err_cap);
int kvspaceMkindexExt(void *h, const char *path, const char *ext_path, char *err, uint32_t err_cap);
int kvspaceRmindexExt(void *h, const char *path, char *err, uint32_t err_cap);
int kvspaceClear(void *h, char *err, uint32_t err_cap);
/* 借用：*out 指向后端常驻空间，调用方不得 free。 */
int kvspaceWatch(void *h, const char *key, const uint8_t *target, uint32_t target_len,
                 uint64_t tick_ns, uint8_t **out, uint32_t *out_len);

/* XValue constructors (stateless, implemented in frontend, byte-identical across backends).
 * Fixed 8-byte header + body + attr region (see kvspace/xvalue_head.h).
 * Empty/NULL langtype = untyped (omits langtype record; attr region is just 2-byte attr_len). */

/* Construct a leaf Ptr XValue (storetype=SLACK, ptr=PTR, body=target path bytes,
 * target_kindexpr written to attr tag=1; "" means unconstrained; rejects "any" and empty target).
 * Pure wire constructor: does not connect to a backend or validate target existence/langtype. */
int kvspaceNewPtr(const char *target_kindexpr, const char *target,
                  uint8_t **out, uint32_t *out_len);
int kvspaceNewChar(const uint8_t *bytes, uint32_t len, const char *langtype,
                   uint8_t **out, uint32_t *out_len);
int kvspaceNewBool(uint8_t v, const char *langtype,
                   uint8_t **out, uint32_t *out_len);
int kvspaceNewInt64(int64_t v, const char *langtype,
                    uint8_t **out, uint32_t *out_len);
int kvspaceNewFloat64(double v, const char *langtype,
                      uint8_t **out, uint32_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
