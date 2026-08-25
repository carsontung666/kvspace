/* frontend.c — KVSpace dispatch 前端。
 *
 * 导出与两个后端完全相同的 25 个 kvspace* 符号。运行期按 DSN scheme 用 dlopen
 * （RTLD_NOW | RTLD_LOCAL）装载后端，把 handle 包一层 {vtable, dl, backend}。
 * codec（TlvEncode、DecodeHead、New 等）无 handle，由前端静态实现，byte-identical。
 *
 * 后端装载名：
 *   shm://...           → libkvspace-c.so.1
 *   其余（redis/fs/s3） → libkvspace_durable.so.1
 * 目录由 KVSPACE_BACKEND_PATH 覆盖（默认走动态链接器搜索路径）。
 */

#include "kvspace/kvspace.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 后端 vtable（仅 handle 相关符号；codec 由前端静态实现） ─────────── */

typedef struct {
    void (*free)(void *h);
    int  (*disconnect)(void *h, char *err, uint32_t err_cap);
    int  (*set)(void *h, const char *const *keys, const uint8_t *vals,
                const uint32_t *lens, uint32_t n, char *err, uint32_t err_cap);
    int  (*get)(void *h, const char *key, uint8_t **out, uint32_t *out_len);
    int  (*getbatch)(void *h, const char *prefix, const char *const *names,
                     uint32_t nnames, uint8_t **out, uint32_t *out_len);
    int  (*list)(void *h, const char *prefix, int expand_ext, int resolve,
                 uint8_t **out, uint32_t *out_len);
    int  (*del)(void *h, const char *const *keys, uint32_t nkeys, char *err, uint32_t err_cap);
    int  (*deltree)(void *h, const char *prefix, char *err, uint32_t err_cap);
    int  (*mkindex)(void *h, const char *path, char *err, uint32_t err_cap);
    int  (*mkindexext)(void *h, const char *path, const char *ext_path, char *err, uint32_t err_cap);
    int  (*rmindexext)(void *h, const char *path, char *err, uint32_t err_cap);
    int  (*clear)(void *h, char *err, uint32_t err_cap);
    int  (*watch)(void *h, const char *key, const uint8_t *target, uint32_t target_len,
                  uint64_t tick_ns, uint8_t **out, uint32_t *out_len);
    int  (*notify)(void *h, const char *key, const uint8_t *val, uint32_t len, char *err, uint32_t err_cap);
    int  (*take)(void *h, const char *key, uint64_t timeout_ns, uint8_t **out, uint32_t *out_len);
    int  (*watchany)(void *h, const char *const *keys, uint32_t nkeys, uint64_t timeout_ns,
                     uint8_t **out_key, uint32_t *out_key_len, uint8_t **out, uint32_t *out_len);
    int  (*incr)(void *h, const char *key, int64_t *out, char *err, uint32_t err_cap);
    int  (*expire)(void *h, const char *key, uint64_t ttl_ns, char *err, uint32_t err_cap);
} kvspace_vt;

typedef struct {
    kvspace_vt *vt;
    void       *dl;
    void       *backend;
} kvspace_handle;

static kvspace_handle *H(void *h) { return (kvspace_handle *)h; }

/* ── 后端选择 ───────────────────────────────────────────────────────── */

static const char *backend_soname(const char *dsn) {
    return (dsn && strncmp(dsn, "shm://", 6) == 0)
        ? "libkvspace-c.so.1"
        : "libkvspace_durable.so.1";
}

static char *backend_path(const char *soname, char *buf, size_t cap) {
    const char *dir = getenv("KVSPACE_BACKEND_PATH");
    if (!dir || !dir[0]) dir = "/usr/lib/kvspace";
    snprintf(buf, cap, "%s/%s", dir, soname);
    return buf;
}

/* ── 生命周期 ───────────────────────────────────────────────────────── */

void *kvspaceConnect(const char *dsn) {
    if (!dsn) return NULL;
    char path[4096];
    backend_path(backend_soname(dsn), path, sizeof path);

    void *dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) return NULL;

    kvspace_vt *vt = calloc(1, sizeof(*vt));
    if (!vt) { dlclose(dl); return NULL; }

    #define LOAD(field, name) do {                                  \
        *(void **)&(vt)->field = dlsym(dl, name);                   \
        if (!(vt)->field) { free(vt); dlclose(dl); return NULL; }   \
    } while (0)

    LOAD(free, "kvspaceClose");
    LOAD(disconnect, "kvspaceDisconnect");
    LOAD(set, "kvspaceSet");
    LOAD(get, "kvspaceGet");
    LOAD(getbatch, "kvspaceGetBatch");
    LOAD(list, "kvspaceList");
    LOAD(del, "kvspaceDel");
    LOAD(deltree, "kvspaceDelTree");
    LOAD(mkindex, "kvspaceMkindex");
    LOAD(mkindexext, "kvspaceMkindexExt");
    LOAD(rmindexext, "kvspaceRmindexExt");
    LOAD(clear, "kvspaceClear");
    LOAD(watch, "kvspaceWatch");
    #undef LOAD
    /* optional: shm backend with Notify/Take/Incr/Expire/WatchAny; durable may omit */
    *(void **)&vt->notify = dlsym(dl, "kvspaceNotify");
    *(void **)&vt->take = dlsym(dl, "kvspaceTake");
    *(void **)&vt->watchany = dlsym(dl, "kvspaceWatchAny");
    *(void **)&vt->incr = dlsym(dl, "kvspaceIncr");
    *(void **)&vt->expire = dlsym(dl, "kvspaceExpire");

    void *(*connect)(const char *) = dlsym(dl, "kvspaceConnect");
    if (!connect) { free(vt); dlclose(dl); return NULL; }
    void *backend = connect(dsn);
    if (!backend) { free(vt); dlclose(dl); return NULL; }

    kvspace_handle *h = malloc(sizeof(*h));
    if (!h) { free(vt); dlclose(dl); return NULL; }
    h->vt = vt; h->dl = dl; h->backend = backend;
    return h;
}

void kvspaceClose(void *h) {
    if (!h) return;
    kvspace_handle *x = H(h);
    if (x->vt->free) x->vt->free(x->backend);
    if (x->dl) dlclose(x->dl);
    free(x->vt);
    free(x);
}

void kvspaceBytesFree(uint8_t *p, uint32_t len) {
    /* 空值（durable 返回悬垂指针 0x1 + len=0）不 free；对齐 durable 的 Box<[u8]>::from_raw 语义。 */
    if (p && len > 0)
        free(p);
}

int kvspaceDisconnect(void *h, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->disconnect ? x->vt->disconnect(x->backend, err, err_cap) : 0;
}

/* ── 单点读写 / 目录（trampoline） ─────────────────────────────────── */

int kvspaceSet(void *h, const char *const *keys, const uint8_t *vals,
               const uint32_t *lens, uint32_t n, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->set(x->backend, keys, vals, lens, n, err, err_cap);
}

int kvspaceGet(void *h, const char *key, uint8_t **out, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    return x->vt->get(x->backend, key, out, out_len);
}

int kvspaceGetBatch(void *h, const char *prefix, const char *const *names,
                    uint32_t nnames, uint8_t **out, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    return x->vt->getbatch(x->backend, prefix, names, nnames, out, out_len);
}

int kvspaceList(void *h, const char *prefix, int expand_ext, int resolve,
                uint8_t **out, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    return x->vt->list(x->backend, prefix, expand_ext, resolve, out, out_len);
}

int kvspaceDel(void *h, const char *const *keys, uint32_t nkeys, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->del(x->backend, keys, nkeys, err, err_cap);
}

int kvspaceDelTree(void *h, const char *prefix, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->deltree(x->backend, prefix, err, err_cap);
}

int kvspaceMkindex(void *h, const char *path, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->mkindex(x->backend, path, err, err_cap);
}

int kvspaceMkindexExt(void *h, const char *path, const char *ext_path, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->mkindexext(x->backend, path, ext_path, err, err_cap);
}

int kvspaceRmindexExt(void *h, const char *path, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->rmindexext(x->backend, path, err, err_cap);
}

int kvspaceClear(void *h, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->clear(x->backend, err, err_cap);
}

int kvspaceWatch(void *h, const char *key, const uint8_t *target, uint32_t target_len,
                 uint64_t tick_ns, uint8_t **out, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    return x->vt->watch(x->backend, key, target, target_len, tick_ns, out, out_len);
}

int kvspaceNotify(void *h, const char *key, const uint8_t *val, uint32_t len, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    if (!x->vt->notify) return 1;
    return x->vt->notify(x->backend, key, val, len, err, err_cap);
}

int kvspaceTake(void *h, const char *key, uint64_t timeout_ns, uint8_t **out, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    if (!x->vt->take) return 1;
    return x->vt->take(x->backend, key, timeout_ns, out, out_len);
}

int kvspaceWatchAny(void *h, const char *const *keys, uint32_t nkeys, uint64_t timeout_ns,
                    uint8_t **out_key, uint32_t *out_key_len, uint8_t **out, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    if (!x->vt->watchany) return 1;
    return x->vt->watchany(x->backend, keys, nkeys, timeout_ns, out_key, out_key_len, out, out_len);
}

int kvspaceIncr(void *h, const char *key, int64_t *out, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    if (!x->vt->incr) return 1;
    return x->vt->incr(x->backend, key, out, err, err_cap);
}

int kvspaceExpire(void *h, const char *key, uint64_t ttl_ns, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    if (!x->vt->expire) return 1;
    return x->vt->expire(x->backend, key, ttl_ns, err, err_cap);
}

/* ── codec（静态，byte-identical 头格式） ──────────────────────────── */

static void wr_u32(uint8_t *d, uint32_t v) {
    d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24);
}
static void wr_u64(uint8_t *d, uint64_t v) {
    for (int i = 0; i < 8; i++) d[i] = (uint8_t)(v >> (i * 8));
}
static uint32_t rd_u32(const uint8_t *d) {
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

static int build_kindexpr(char *buf, size_t cap, const char *kind, int ref,
                          const int32_t *dims, int ndim) {
    int o = 0;
    if (ref == 1) buf[o++] = '*';
    else if (ref == 2) buf[o++] = '@';
    if (ndim > 0) {
        buf[o++] = '[';
        for (int i = 0; i < ndim; i++) {
            if (i) buf[o++] = ',';
            o += snprintf(buf + o, cap - (size_t)o, "%d", dims[i]);
        }
        buf[o++] = ']';
    }
    size_t kl = strlen(kind);
    memcpy(buf + o, kind, kl);
    return o + (int)kl;
}

static int encode_head(const char *kind, int ref, int ro, uint32_t vid,
                       const int32_t *dims, int ndim,
                       const uint8_t *raw, uint32_t raw_len,
                       uint8_t **out, uint32_t *out_len) {
    char kx[256];
    int kxl = build_kindexpr(kx, sizeof kx, kind, ref, dims, ndim);
    int slot = kxl + 1;
    uint32_t total = 1u + (uint32_t)slot + 1u + 4u + 4u + raw_len;
    uint8_t *buf = malloc(total);
    if (!buf) return 1;
    buf[0] = (uint8_t)slot;
    memcpy(buf + 1, kx, (size_t)kxl);
    buf[1 + kxl] = 0;
    int o = 1 + slot;
    buf[o] = (uint8_t)(ro ? 1 : 0);
    wr_u32(buf + o + 1, vid);
    wr_u32(buf + o + 5, raw_len);
    if (raw_len && raw) memcpy(buf + o + 9, raw, raw_len);
    *out = buf; *out_len = total;
    return 0;
}

/* array_len → dims：char/* 恒一维；其余 >1 才一维，否则标量。 */
static int al_to_dims(const char *kind, int32_t array_len, int32_t *dims) {
    if (strncmp(kind, "char/", 5) == 0) { dims[0] = array_len < 0 ? 0 : array_len; return 1; }
    if (array_len > 1) { dims[0] = array_len; return 1; }
    return 0;
}

static uint32_t utf8_next(const uint8_t *s, int len, int *i) {
    uint32_t cp = s[*i];
    if (cp < 0x80) { (*i)++; return cp; }
    int n;
    if ((cp & 0xE0) == 0xC0)      { n = 1; cp &= 0x1F; }
    else if ((cp & 0xF0) == 0xE0) { n = 2; cp &= 0x0F; }
    else if ((cp & 0xF8) == 0xF0) { n = 3; cp &= 0x07; }
    else { (*i)++; return 0xFFFD; }
    (*i)++;
    for (int j = 0; j < n && *i < len; j++, (*i)++) cp = (cp << 6) | (s[*i] & 0x3F);
    return cp;
}

int kvspaceTlvEncode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                     const int32_t *dims, int32_t ndim, uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len || !kind) return 1;
    if (ndim < 0) ndim = 0;
    if (ndim > 8) return 1;
    return encode_head(kind, 0, 0, 0, dims, ndim, raw, raw_len, out, out_len);
}

int kvspaceTlvEncodePtr(const char *kind, const uint8_t *raw, uint32_t raw_len,
                        const int32_t *dims, int32_t ndim, uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len || !kind) return 1;
    if (ndim < 0) ndim = 0;
    if (ndim > 8) return 1;
    return encode_head(kind, 1, 0, 0, dims, ndim, raw, raw_len, out, out_len);
}

int kvspaceTlvEncodeMode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                         const int32_t *dims, int32_t ndim, int32_t ref, uint8_t ro, uint32_t vid,
                         uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len || !kind) return 1;
    if (ndim < 0) ndim = 0;
    if (ndim > 8) return 1;
    return encode_head(kind, ref, ro ? 1 : 0, vid, dims, ndim, raw, raw_len, out, out_len);
}

int kvspaceDecodeHead(const uint8_t *data, uint32_t data_len, kvspaceHead_t *out) {
    if (!out) return 1;
    memset(out, 0, sizeof(*out));
    if (!data || data_len < 1) return 1;
    int slot = data[0];
    int o = 1 + slot;
    if ((uint32_t)o + 9 > data_len) return 1;
    const uint8_t *kx = data + 1;
    int kxl = 0;
    while (kxl < slot && kx[kxl] != 0) kxl++;
    int n = kxl > 255 ? 255 : kxl;
    memcpy(out->kindexpr, kx, (size_t)n);
    out->kindexpr[n] = 0;
    out->ro = data[o] & 1;
    out->vid = rd_u32(data + o + 1);
    out->body_len = (int32_t)rd_u32(data + o + 5);
    out->body_offset = o + 9;
    return 0;
}

int kvspaceNewPtr(const char *kind, const char *target, int32_t array_len,
                  uint8_t **out, uint32_t *out_len) {
    if (!kind || !target || !out || !out_len) return 1;
    if (array_len <= 0) array_len = 1;
    int32_t dims[1]; int ndim = al_to_dims(kind, array_len, dims);
    return encode_head(kind, 1, 0, 0, ndim ? dims : NULL, ndim,
                       (const uint8_t *)target, (uint32_t)strlen(target), out, out_len);
}

int kvspaceNewChar(const char *kind, const char *s, uint8_t **out, uint32_t *out_len) {
    if (!kind || !s || !out || !out_len) return 1;
    if (strcmp(kind, "char/utf32") != 0) {
        size_t sl = strlen(s);
        int32_t d[1] = { (int32_t)sl };
        return encode_head(kind, 0, 0, 0, d, 1, (const uint8_t *)s, (uint32_t)sl, out, out_len);
    }
    /* char/utf32：UTF-8 → UTF-32 LE 码点 */
    int slen = (int)strlen(s);
    uint8_t *raw = malloc((size_t)slen * 4);
    if (!raw) return 1;
    int n = 0, i = 0;
    while (i < slen) { uint32_t cp = utf8_next((const uint8_t *)s, slen, &i); wr_u32(raw + n * 4, cp); n++; }
    int32_t d[1] = { n };
    int rc = encode_head("char/utf32", 0, 0, 0, d, 1, raw, (uint32_t)(n * 4), out, out_len);
    free(raw);
    return rc;
}

int kvspaceNewCharByte(const uint8_t *bytes, uint32_t len, uint8_t **out, uint32_t *out_len) {
    if (!bytes || !out || !out_len) return 1;
    int32_t d[1] = { (int32_t)len };
    return encode_head("char/utf8", 0, 0, 0, d, 1, bytes, len, out, out_len);
}

int kvspaceNewBool(uint8_t v, uint8_t **out, uint32_t *out_len) {
    uint8_t b = v ? 1 : 0;
    return encode_head("bool", 0, 0, 0, NULL, 0, &b, 1, out, out_len);
}

int kvspaceNewInt64(int64_t v, uint8_t **out, uint32_t *out_len) {
    uint8_t b[8]; wr_u64(b, (uint64_t)v);
    return encode_head("int64", 0, 0, 0, NULL, 0, b, 8, out, out_len);
}

int kvspaceNewFloat64(double v, uint8_t **out, uint32_t *out_len) {
    uint8_t b[8];
    uint64_t bits; memcpy(&bits, &v, 8);
    wr_u64(b, bits);
    return encode_head("float64", 0, 0, 0, NULL, 0, b, 8, out, out_len);
}
