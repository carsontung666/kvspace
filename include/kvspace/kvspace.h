/*
 * kvspace.h — KVSpace C ABI（权威定义）
 *
 * 这是 kvspace-c（SHM）与 kvspace-durable（redis/fs/s3）两个后端共同实现的
 * 唯一 C ABI。消费者只链接 libkvspace（dispatch 前端），运行期按 DSN scheme 选择后端：
 *   shm://...      → libkvspace-c.so.1
 *   其余（redis/fs/s3）→ libkvspace_durable.so.1
 *
 * 头格式（byte-identical，两边一致，由前端静态实现 codec）：
 *   [1B kindexprlen][kindexpr 含 0x00 padding][1B ro][4B vid LE][4B raw_len LE][raw]
 *   kindexpr 串首字节 * =软链接(Ptr) / @ =扩展句柄 / 无 =内联，其后 [d0,d1]kind 承载 ndim+dims。
 */

#ifndef KVSPACE_H
#define KVSPACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* XValue 头（repr C）。kindexpr 为唯一类型真相，body 靠 offset/len 定位。 */
typedef struct {
    uint8_t  kindexpr[256]; /* NUL 终止（含 ref 前缀与 [dims]，去 padding） */
    uint8_t  ro;            /* 1=只读，0=可写 */
    uint32_t vid;           /* vthread id（默认 0） */
    int32_t  body_len;      /* body 字节数 */
    int32_t  body_offset;   /* body 在 data 内的起始偏移（= head 长度） */
} kvspaceHead_t;

/* ── 生命周期 ─────────────────────────────────────────────────── */
void *kvspaceConnect(const char *dsn);
void  kvspaceClose(void *h);
void  kvspaceBytesFree(uint8_t *p, uint32_t len);
int   kvspaceDisconnect(void *h, char *err, uint32_t err_cap);

/* ── 单点读写 / 目录 ──────────────────────────────────────────── */
int kvspaceSet(void *h, const char *const *keys, const uint8_t *vals,
               const uint32_t *lens, uint32_t n, char *err, uint32_t err_cap);
int kvspaceGet(void *h, const char *key, uint8_t **out, uint32_t *out_len);
int kvspaceGetBatch(void *h, const char *prefix, const char *const *names,
                    uint32_t nnames, uint8_t **out, uint32_t *out_len);
int kvspaceList(void *h, const char *prefix, int expand_ext, int resolve,
                uint8_t **out, uint32_t *out_len);
int kvspaceDel(void *h, const char *const *keys, uint32_t nkeys, char *err, uint32_t err_cap);
int kvspaceDelTree(void *h, const char *prefix, char *err, uint32_t err_cap);
int kvspaceMkindex(void *h, const char *path, char *err, uint32_t err_cap);
int kvspaceMkindexExt(void *h, const char *path, const char *ext_path, char *err, uint32_t err_cap);
int kvspaceRmindexExt(void *h, const char *path, char *err, uint32_t err_cap);
int kvspaceClear(void *h, char *err, uint32_t err_cap);
int kvspaceWatch(void *h, const char *key, const uint8_t *target, uint32_t target_len,
                 uint64_t tick_ns, uint8_t **out, uint32_t *out_len);
int kvspaceNotify(void *h, const char *key, const uint8_t *val, uint32_t len, char *err, uint32_t err_cap);
int kvspaceTake(void *h, const char *key, uint64_t timeout_ns, uint8_t **out, uint32_t *out_len);
int kvspaceWatchAny(void *h, const char *const *keys, uint32_t nkeys, uint64_t timeout_ns,
                    uint8_t **out_key, uint32_t *out_key_len, uint8_t **out, uint32_t *out_len);
int kvspaceIncr(void *h, const char *key, int64_t *out, char *err, uint32_t err_cap);
int kvspaceExpire(void *h, const char *key, uint64_t ttl_ns, char *err, uint32_t err_cap);

/* ── codec（无 handle，由前端静态实现，byte-identical） ─────────── */
int kvspaceTlvEncode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                     const int32_t *dims, int32_t ndim, uint8_t **out, uint32_t *out_len);
int kvspaceTlvEncodePtr(const char *kind, const uint8_t *raw, uint32_t raw_len,
                        const int32_t *dims, int32_t ndim, uint8_t **out, uint32_t *out_len);
int kvspaceTlvEncodeMode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                         const int32_t *dims, int32_t ndim, int32_t ref, uint8_t ro, uint32_t vid,
                         uint8_t **out, uint32_t *out_len);
int kvspaceDecodeHead(const uint8_t *data, uint32_t data_len, kvspaceHead_t *out);

int kvspaceNewPtr(const char *kind, const char *target, int32_t array_len,
                  uint8_t **out, uint32_t *out_len);
int kvspaceNewChar(const char *kind, const char *s, uint8_t **out, uint32_t *out_len);
int kvspaceNewCharByte(const uint8_t *bytes, uint32_t len, uint8_t **out, uint32_t *out_len);
int kvspaceNewBool(uint8_t v, uint8_t **out, uint32_t *out_len);
int kvspaceNewInt64(int64_t v, uint8_t **out, uint32_t *out_len);
int kvspaceNewFloat64(double v, uint8_t **out, uint32_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
