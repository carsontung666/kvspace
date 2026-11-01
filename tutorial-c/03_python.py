#!/usr/bin/env python3
"""03_python — ctypes 直调 libkvspace（dispatch 前端），shm:// 后端
#571: Fixed 8-byte header (body offset is always 8); langtype in inline attr region."""

import ctypes
import os
import struct

SO = os.environ.get("KVSPACE_SO", "libkvspace.so")
lib = ctypes.CDLL(SO)

U8P = ctypes.POINTER(ctypes.c_uint8)
U32P = ctypes.POINTER(ctypes.c_uint32)
U64P = ctypes.POINTER(ctypes.c_uint64)
CCHARPP = ctypes.POINTER(ctypes.c_char_p)

KVSPACE_XVALUE_HEAD_LEN = 8


# Matches kvspaceHead_t (langtype populated by kvspaceGetHead from inline attr).
class Head(ctypes.Structure):
    _fields_ = [
        ("headlen", ctypes.c_uint16),
        ("ref", ctypes.c_uint8),
        ("storetype", ctypes.c_uint8),
        ("body_len", ctypes.c_int32),
        ("ndim", ctypes.c_int32),
        ("dims", ctypes.c_int32 * 8),
        ("langtype", ctypes.c_char * 256),
        ("langtype_len", ctypes.c_int32),
        ("body_offset", ctypes.c_int32),
        ("body_cap", ctypes.c_uint64),
    ]


# 固定 8 字节 head：[len:u48 LE@0-5][storetype@6][ptr@7]。
class XvalueHead(ctypes.Structure):
    _fields_ = [
        ("len", ctypes.c_uint8 * 6),
        ("storetype", ctypes.c_uint8),
        ("ptr", ctypes.c_uint8),
    ]


def xvalue_head_len(h):
    v = 0
    for i in reversed(range(6)):
        v = (v << 8) | h.len[i]
    return v


def bind(fn, args, restype):
    fn.argtypes = args
    fn.restype = restype


bind(lib.kvspaceConnect, [ctypes.c_char_p], ctypes.c_void_p)
bind(lib.kvspaceClose, [ctypes.c_void_p], None)
# 借用读：out 指向 kvspace 常驻空间，string_at 拷出后不 free。resolve 穿透 link。
bind(lib.kvspaceGet, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(U8P), U32P], ctypes.c_int)
bind(lib.kvspaceGetHead, [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(Head)], ctypes.c_int)
# 写即构造：就地/新位置二选一原语，往返回的 body 偏移指针写字节。
bind(lib.kvspaceWriteInPlace, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_uint32, ctypes.POINTER(U8P), ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
# Signature matches kvspace.h: (h, key, ref, storetype, body_len, body_cap, langtype, body, err, err_cap).
bind(lib.kvspaceWriteNewPlace, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint8, ctypes.c_uint8,
                                ctypes.c_uint32, ctypes.c_uint64, ctypes.c_char_p,
                                ctypes.POINTER(U8P), ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
# 前缀枚举：ListLen 定计数 + 逐 idx ListAt 借用取名。
bind(lib.kvspaceListLen, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int32)], ctypes.c_int)
bind(lib.kvspaceListAt, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int32,
                         U8P, ctypes.c_uint32, U32P], ctypes.c_int)
bind(lib.kvspaceDel, [ctypes.c_void_p, CCHARPP, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
bind(lib.kvspaceDelTree, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
bind(lib.kvspaceMkindex, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
# New*: langtype is stored in inline attr (empty/None = untyped).
bind(lib.kvspaceNewInt64, [ctypes.c_int64, ctypes.c_char_p, ctypes.POINTER(U8P), U32P], ctypes.c_int)
bind(lib.kvspaceNewChar, [U8P, ctypes.c_uint32, ctypes.c_char_p, ctypes.POINTER(U8P), U32P], ctypes.c_int)
bind(lib.free, [ctypes.c_void_p], None)


def enc_int(v):
    out, n = U8P(), ctypes.c_uint32()
    if lib.kvspaceNewInt64(v, b"int64", ctypes.byref(out), ctypes.byref(n)) != 0:
        raise RuntimeError("kvspaceNewInt64 failed")
    data = ctypes.string_at(out, n.value)
    lib.free(out)  # allocated by frontend malloc
    return data


def new_str(s):
    b = s.encode()
    buf = (ctypes.c_uint8 * len(b)).from_buffer_copy(b)
    out, n = U8P(), ctypes.c_uint32()
    if lib.kvspaceNewChar(buf, len(b), b"char/utf8", ctypes.byref(out), ctypes.byref(n)) != 0:
        raise RuntimeError("kvspaceNewChar failed")
    data = ctypes.string_at(out, n.value)
    lib.free(out)  # allocated by frontend malloc
    return data


# Read kind via kvspaceGetHead (from inline attr); body starts at offset 8 with length h.body_len.
def read_kind(kv, key, data):
    h = Head()
    if lib.kvspaceGetHead(kv, key.encode(), ctypes.byref(h)) != 0:
        return "", b""
    kx = bytes(h.langtype).split(b"\0", 1)[0].decode()
    if kx.startswith(("*", "@")):
        kx = kx[1:]
    if kx.startswith("["):
        kx = kx[kx.index("]") + 1:]
    bl = h.body_len if h.body_len > 0 else 0
    return kx, data[KVSPACE_XVALUE_HEAD_LEN:KVSPACE_XVALUE_HEAD_LEN + bl]


class KV:
    def __init__(self, path):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        self.path = path
        self.kv = lib.kvspaceConnect(f"shm://{path}".encode())
        if not self.kv:
            raise RuntimeError(f"connect shm://{path} failed")

    def close(self):
        lib.kvspaceClose(self.kv)
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    # Write into returned body pointer; WriteNewPlace writes langtype with the header/attr region.
    def set(self, key, langtype, val):
        if len(val) < KVSPACE_XVALUE_HEAD_LEN:
            return
        buf = (ctypes.c_uint8 * len(val)).from_buffer_copy(val)
        hd = XvalueHead()
        ctypes.memmove(ctypes.byref(hd), buf, ctypes.sizeof(hd))
        bl = xvalue_head_len(hd)
        dst = U8P()
        err = ctypes.create_string_buffer(256)
        if lib.kvspaceWriteInPlace(self.kv, key.encode(), 1, bl, ctypes.byref(dst), err, 256) != 0:
            if lib.kvspaceWriteNewPlace(self.kv, key.encode(), hd.ptr, hd.storetype,
                                        bl, bl, langtype.encode(), ctypes.byref(dst), err, 256) != 0:
                raise RuntimeError(f"write {key}: {err.value.decode(errors='replace')}")
        if bl > 0 and dst:
            ctypes.memmove(dst, ctypes.byref(buf, KVSPACE_XVALUE_HEAD_LEN), bl)

    def get(self, key):
        out, n = U8P(), ctypes.c_uint32()
        lib.kvspaceGet(self.kv, key.encode(), 0, ctypes.byref(out), ctypes.byref(n))
        if not out or n.value == 0:
            return None
        return ctypes.string_at(out, n.value)

    def delete(self, key):
        keys = (ctypes.c_char_p * 1)(key.encode())
        lib.kvspaceDel(self.kv, keys, 1, None, 0)

    def deltree(self, prefix):
        lib.kvspaceDelTree(self.kv, prefix.encode(), None, 0)

    def mkindex(self, path):
        lib.kvspaceMkindex(self.kv, path.encode(), 0, None, 0)

    def list(self, prefix):
        count = ctypes.c_int32()
        if lib.kvspaceListLen(self.kv, prefix.encode(), 0, 1, ctypes.byref(count)) != 0 or count.value <= 0:
            return []
        names = []
        for i in range(count.value):
            out = (ctypes.c_uint8 * 1024)()
            n = ctypes.c_uint32()
            if lib.kvspaceListAt(self.kv, prefix.encode(), 0, 1, i, out, 1024, ctypes.byref(n)) == 0 and n.value > 0:
                names.append(ctypes.string_at(out, n.value).decode())
        return names


with KV("/tmp/kvspace_py.shm") as kv:
    kv.mkindex("/py/")

    kv.set("/py/a", "int64", enc_int(42))
    kv.set("/py/b", "char/utf8", new_str("你好"))

    for k in ["/py/a", "/py/b"]:
        kind, raw = read_kind(kv.kv, k, kv.get(k))
        if kind == "int64":
            val = struct.unpack("<q", raw)[0]
        else:
            val = raw.decode()
        print(f"{k}\t{kind}:{val}")

    ns = kv.list("/py/")
    print(f"list /py/: {sorted(ns)} (count={len(ns)})")

    kv.delete("/py/b")
    assert len(kv.list("/py/")) == 1
    kv.deltree("/py/")
    assert kv.list("/py/") == []

print("PASS python")
