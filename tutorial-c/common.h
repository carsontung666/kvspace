// common.h — tutorial 公共助手：统一 kvspace* ABI 的 XValue 构造/读取与单键读写便捷封装。
// 所有 tutorial 均链接 libkvspace（dispatch 前端），运行期经 shm:// 选择 kvspace-c 后端。
// #571: Fixed 8-byte header (body offset is always 8); langtype (incl. [dims]) in inline attr.
#ifndef KVSPACE_TUTORIAL_COMMON_H
#define KVSPACE_TUTORIAL_COMMON_H

#include "kvspace/kvspace.h"
#include <kvspace/xvalue_head.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Constructors: return malloc-allocated XValue bytes (header + body + attr); caller frees.

static inline uint8_t *new_int64(int64_t v, uint32_t *len) {
    uint8_t *out = NULL; uint32_t n = 0;
    kvspaceNewInt64(v, "int64", &out, &n);
    *len = n; return out;
}

static inline uint8_t *new_str(const char *s, uint32_t *len) {
    uint8_t *out = NULL; uint32_t n = 0;
    kvspaceNewChar((const uint8_t *)s, (uint32_t)strlen(s), "char/utf8", &out, &n);
    *len = n; return out;
}

static inline uint8_t *new_bytes(const uint8_t *raw, uint32_t rl, uint32_t *len) {
    uint8_t *out = NULL; uint64_t n = 0;
    uint64_t dims[1] = { rl };
    kvspaceXvalueHeadNewTensor(dims, 1, raw, rl, NULL, &out, &n);
    *len = n > UINT32_MAX ? 0 : (uint32_t)n;
    return out;
}

// Reader: bounded langtype parse from inline attr (stripping leading [dims] for base kind);
// body starts at offset 8 with length from header.

static inline const char *read_kind(const uint8_t *data, uint32_t len,
                                    char *kind, size_t cap,
                                    const uint8_t **body, uint32_t *body_len) {
    if (cap) kind[0] = 0;
    *body = NULL; *body_len = 0;
    if (!data || len < KVSPACE_XVALUE_HEAD_LEN || cap == 0) return NULL;
    kvspaceXvalueHead hd;
    memcpy(&hd, data, sizeof hd);
    uint64_t bl = kvspaceXvalueHeadLen(&hd);
    if (bl > len - KVSPACE_XVALUE_HEAD_LEN) return NULL;
    const uint8_t *lt = NULL;
    size_t n = kvspaceXvalueHeadLangtypeBounded(data, len, &lt);
    const uint8_t *e = lt && n && lt[0] == '[' ? memchr(lt, ']', n) : NULL;
    if (e) { n -= (size_t)(e + 1 - lt); lt = e + 1; }
    if (n >= cap) n = cap - 1;
    if (n) memcpy(kind, lt, n);
    kind[n] = 0;
    *body = data + KVSPACE_XVALUE_HEAD_LEN;
    *body_len = (uint32_t)bl;
    return kind;
}

// Single-key read/write helpers (WriteInPlace or WriteNewPlace with inline langtype).

static inline int kv_set(void *h, const char *key, const char *langtype,
                         const uint8_t *val, uint32_t len) {
    if (!val || len < KVSPACE_XVALUE_HEAD_LEN) return -1;
    kvspaceXvalueHead hd;
    memcpy(&hd, val, sizeof hd);
    uint64_t bl = kvspaceXvalueHeadLen(&hd);
    uint8_t *dst = NULL; char err[256];
    if (kvspaceWriteInPlace(h, key, 1, (uint32_t)bl, &dst, err, sizeof err) != 0 &&
        kvspaceWriteNewPlace(h, key, hd.ptr, hd.storetype, (uint32_t)bl, bl,
                             langtype, &dst, err, sizeof err) != 0)
        return -1;
    if (bl && dst) memcpy(dst, val + KVSPACE_XVALUE_HEAD_LEN, (size_t)bl);
    return 0;
}

// 借用读 + 拷出自持：调用方以 libc free 释放（借用本身不得 free）。
static inline uint8_t *kv_get(void *h, const char *key, uint32_t *len) {
    uint8_t *out = NULL; uint32_t n = 0;
    kvspaceGet(h, key, 0, &out, &n);
    if (!out || n == 0) { *len = 0; return NULL; }
    uint8_t *copy = malloc(n);
    memcpy(copy, out, n);
    *len = n; return copy;
}

// 前缀枚举：ListLen 定计数 + 逐 idx ListAt 取名（自备缓冲），拼成 \n 分隔的 malloc 缓冲（调用方 free）。
static inline int kv_list(void *h, const char *prefix, uint8_t **out, uint32_t *out_len) {
    *out = NULL; *out_len = 0;
    int32_t count = 0;
    if (kvspaceListLen(h, prefix, 0, 1, &count) != 0 || count <= 0) return 0;
    size_t cap = 256, used = 0;
    uint8_t *buf = malloc(cap);
    for (int32_t i = 0; i < count; i++) {
        uint8_t nm[1024]; uint32_t nl = 0;
        if (kvspaceListAt(h, prefix, 0, 1, i, nm, sizeof(nm), &nl) != 0 || nl == 0) continue;
        if (used + nl + 1 > cap) { while (used + nl + 1 > cap) cap *= 2; buf = realloc(buf, cap); }
        if (used > 0) buf[used++] = '\n';
        memcpy(buf + used, nm, nl); used += nl;
    }
    *out = buf; *out_len = (uint32_t)used;
    return 0;
}

static inline void kv_del(void *h, const char *key) {
    const char *keys[1] = { key };
    kvspaceDel(h, keys, 1, NULL, 0);
}

// ── 连接：shm:// DSN → dispatch 前端 → kvspace-c 后端 ──

static inline void *kv_open_shm(const char *path) {
    size_t n = strlen(path) + 7;
    char *dsn = malloc(n);
    snprintf(dsn, n, "shm://%s", path);
    void *h = kvspaceConnect(dsn);
    free(dsn);
    return h;
}

#endif
