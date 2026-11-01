#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Run tutorial-durable/*.sh and compare against expected header comments;
cross-check XValue constructors (New*) and header decoding (GetHead) between
kvspace-c and kvspace-durable; and verify write flush on next op and exit (issue #23)."""

import ctypes
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent  # kvspace/
WS = ROOT.parent  # array2d/


def find_first(env_key, candidates):
    override = os.environ.get(env_key)
    if override:
        return Path(override)
    for c in candidates:
        p = Path(c)
        if p.exists():
            return p
    return Path(candidates[0])


DURABLE_SO = find_first("KVSPACE_DURABLE_SO", [
    WS / "kvspace-durable" / "target" / "release" / "libkvspace_durable.so",
    "/usr/lib/kvspace/libkvspace_durable.so.1",
])
KVSPACE_C_DIR = WS / "kvspace-c"
KVSPACE_C_SO = find_first("KVSPACE_C_SO", [
    KVSPACE_C_DIR / "build" / "libkvspace-c.so",
    "/usr/lib/kvspace/libkvspace-c.so.1",
])
FRONTEND_SO = str(find_first("KVSPACE_SO", [
    ROOT / "build" / "libkvspace.so.1",
    "/usr/lib/kvspace/libkvspace.so.1",
]))
KVSPACE_BIN = str(find_first("KVSPACE_BIN", [
    ROOT / "cli" / "target" / "release" / "kvspace",
    os.path.expanduser("~/.local/bin/kvspace"),
    shutil.which("kvspace") or "/usr/bin/kvspace",
]))


def extract_expected(script):
    """从脚本头部 # expected: ... # /end 提取预期输出行。"""
    lines = []
    in_block = False
    with open(script) as f:
        for raw in f:
            line = raw.rstrip('\n')
            if in_block:
                if line == '# /end':
                    break
                if line.startswith('# '):
                    lines.append(line[2:])
                elif line == '#':
                    lines.append('')
            elif line == '# expected:':
                in_block = True
    return lines


def run_script(script):
    """Run script (each cleans up its own prefix via deltree without clearing shared redis) and return stdout lines."""
    env = os.environ.copy()
    env['KVSPACE_BIN'] = KVSPACE_BIN
    if 'KVSPACE' not in env:
        with tempfile.TemporaryDirectory(prefix='kvspace-tut-') as tmp:
            env['KVSPACE'] = f'fs://{tmp}/store'
            r = subprocess.run(['bash', script], capture_output=True, text=True, timeout=30, env=env)
            return r.stdout.rstrip('\n').split('\n') if r.stdout.strip() else [], r
    # Safe to clear explicit fs:// paths; redis:// relies on per-script deltree prefix cleanup.
    if env['KVSPACE'].startswith('fs://'):
        subprocess.run([KVSPACE_BIN, 'clear'], capture_output=True, timeout=10, env=env)
    r = subprocess.run(['bash', script], capture_output=True, text=True, timeout=30, env=env)
    return (r.stdout.rstrip('\n').split('\n') if r.stdout.strip() else []), r


def test_script(script):
    expected = extract_expected(script)
    actual, r = run_script(script)
    if r.returncode == 0 and expected == actual:
        print(f'PASS  {os.path.basename(script)}')
        return True
    print(f'FAIL  {os.path.basename(script)} (rc={r.returncode})')
    print(f'  expected ({len(expected)} lines): {expected[:3]}...' if len(expected) > 3 else f'  expected: {expected}')
    print(f'  actual   ({len(actual)} lines):   {actual[:3]}...' if len(actual) > 3 else f'  actual:   {actual}')
    if r.stderr.strip():
        print(f'  stderr: {r.stderr.strip()[:300]}')
    return False


# -- kvspace-c <-> kvspace-durable cross-check (ctypes, #571 v0.3.23+ ABI byte alignment) --

class HeadV(ctypes.Structure):
    # Matches kvspaceHead_t field layout in kvspace/include/kvspace/kvspace.h.
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


U8P = ctypes.POINTER(ctypes.c_uint8)
U32P = ctypes.POINTER(ctypes.c_uint32)
U8PP = ctypes.POINTER(U8P)

_libc = ctypes.CDLL(None)
_libc.free.argtypes = [ctypes.c_void_p]


def setup_lib(lib):
    lib.kvspaceConnect.argtypes = [ctypes.c_char_p]
    lib.kvspaceConnect.restype = ctypes.c_void_p
    lib.kvspaceClose.argtypes = [ctypes.c_void_p]
    lib.kvspaceClose.restype = None
    lib.kvspaceSetValue.argtypes = [ctypes.c_void_p, ctypes.c_char_p, U8P, ctypes.c_uint32,
                                    ctypes.c_char_p, ctypes.c_uint32]
    lib.kvspaceSetValue.restype = ctypes.c_int
    lib.kvspaceGet.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, U8PP, U32P]
    lib.kvspaceGet.restype = ctypes.c_int
    lib.kvspaceGetHead.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(HeadV)]
    lib.kvspaceGetHead.restype = ctypes.c_int
    lib.kvspaceWriteNewPlace.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint8, ctypes.c_uint8,
        ctypes.c_uint32, ctypes.c_uint64, ctypes.c_char_p,
        U8PP, ctypes.c_char_p, ctypes.c_uint32,
    ]
    lib.kvspaceWriteNewPlace.restype = ctypes.c_int
    lib.kvspaceNewInt64.argtypes = [ctypes.c_int64, ctypes.c_char_p, U8PP, U32P]
    lib.kvspaceNewInt64.restype = ctypes.c_int
    lib.kvspaceNewFloat64.argtypes = [ctypes.c_double, ctypes.c_char_p, U8PP, U32P]
    lib.kvspaceNewFloat64.restype = ctypes.c_int
    lib.kvspaceNewBool.argtypes = [ctypes.c_uint8, ctypes.c_char_p, U8PP, U32P]
    lib.kvspaceNewBool.restype = ctypes.c_int
    lib.kvspaceNewChar.argtypes = [U8P, ctypes.c_uint32, ctypes.c_char_p, U8PP, U32P]
    lib.kvspaceNewChar.restype = ctypes.c_int
    lib.kvspaceNewPtr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, U8PP, U32P]
    lib.kvspaceNewPtr.restype = ctypes.c_int


def call_new(lib, kind, val, langtype):
    out = U8P()
    out_len = ctypes.c_uint32()
    lt = None if langtype is None else langtype.encode()
    if kind == "int64":
        rc = lib.kvspaceNewInt64(val, lt, ctypes.byref(out), ctypes.byref(out_len))
    elif kind == "float64":
        rc = lib.kvspaceNewFloat64(val, lt, ctypes.byref(out), ctypes.byref(out_len))
    elif kind == "bool":
        rc = lib.kvspaceNewBool(val, lt, ctypes.byref(out), ctypes.byref(out_len))
    elif kind == "char":
        arr = (ctypes.c_uint8 * len(val)).from_buffer_copy(val) if val else None
        rc = lib.kvspaceNewChar(arr, len(val), lt, ctypes.byref(out), ctypes.byref(out_len))
    elif kind == "ptr":
        rc = lib.kvspaceNewPtr(lt, val.encode(), ctypes.byref(out), ctypes.byref(out_len))
    else:
        raise ValueError(kind)
    assert rc == 0, f"New({kind}, {langtype}) rc={rc}"
    data = ctypes.string_at(out, out_len.value)
    # Only frontend and C outputs use libc malloc; avoid calling libc free on Rust cdylib Box<[u8]>.
    return data


def put_and_head(lib, h, key, ref, storetype, raw, langtype):
    err = ctypes.create_string_buffer(256)
    body = U8P()
    lt = langtype.encode() if langtype else b""
    rc = lib.kvspaceWriteNewPlace(h, key.encode(), ref, storetype, len(raw), len(raw),
                                  lt, ctypes.byref(body), err, 256)
    assert rc == 0, f"WriteNewPlace({key}) rc={rc}: {err.value}"
    if raw:
        ctypes.memmove(body, raw, len(raw))
    out, out_len = U8P(), ctypes.c_uint32()
    assert lib.kvspaceGet(h, key.encode(), 0, ctypes.byref(out), ctypes.byref(out_len)) == 0
    wire = ctypes.string_at(out, out_len.value)
    hd = HeadV()
    assert lib.kvspaceGetHead(h, key.encode(), ctypes.byref(hd)) == 0
    meta = {
        "headlen": hd.headlen,
        "ref": hd.ref,
        "storetype": hd.storetype,
        "body_len": hd.body_len,
        "langtype": bytes(hd.langtype).split(b"\0", 1)[0].decode(),
        "langtype_len": hd.langtype_len,
        "body_offset": hd.body_offset,
    }
    return wire, meta


def test_kvspace_c_alignment():
    """Verify kvspace-c and kvspace-durable produce byte-identical wire values and HeadV metadata."""
    if not DURABLE_SO.exists():
        subprocess.run(["cargo", "build", "--release"], cwd=WS / "kvspace-durable",
                       check=True, capture_output=True)
    if not KVSPACE_C_SO.exists():
        subprocess.run(["make", "kvspace-c"], cwd=KVSPACE_C_DIR / "build",
                       check=True, capture_output=True)

    lib_d = ctypes.CDLL(str(DURABLE_SO))
    try:
        lib_c = ctypes.CDLL(str(KVSPACE_C_SO))
    except OSError:
        # Preload local shared blockmalloc/slotsboxmalloc only when dynamically linked.
        for dep in ["blockmalloc/build/libblockmalloc.so.1", "slotsboxmalloc/build/libslotsboxmalloc.so"]:
            dep_path = KVSPACE_C_DIR.parent / dep
            if dep_path.exists():
                ctypes.CDLL(str(dep_path), mode=ctypes.RTLD_GLOBAL)
        lib_c = ctypes.CDLL(str(KVSPACE_C_SO))
    setup_lib(lib_d)
    setup_lib(lib_c)

    new_cases = [
        ("int64", 42, "int64"),
        ("int64", -7, ""),
        ("float64", 3.14, "float64"),
        ("bool", 1, "bool"),
        ("char", b"hello", "[]char/utf8"),
        ("ptr", "/target", "int64"),
        ("ptr", "/untyped", ""),
    ]
    ok = True
    for kind, val, lt in new_cases:
        label = f"align New({kind}, langtype={lt!r})"
        bd = call_new(lib_d, kind, val, lt)
        bc = call_new(lib_c, kind, val, lt)
        if bd != bc:
            print(f"FAIL  {label} (bytes differ: {bd.hex()} vs {bc.hex()})")
            ok = False
        else:
            print(f"PASS  {label}")

    with tempfile.TemporaryDirectory(prefix="kvspace-align-") as tmp:
        hd = lib_d.kvspaceConnect(f"fs://{tmp}/fs".encode())
        hc = lib_c.kvspaceConnect(f"shm://{tmp}/c.shm".encode())
        assert hd and hc, "connect backend handles failed"
        try:
            store_cases = [
                ("/s_i64", 0, 0, struct.pack("<q", 42), "int64"),
                ("/s_str", 0, 1, b"hello", "[5]char/utf8"),
                ("/s_t2d", 0, 2, struct.pack("<6i", 1, 2, 3, 4, 5, 6), "[2,3]int32"),
                ("/s_ext", 2, 3, b"s3://bucket/obj", "int64"),
            ]
            for key, ref, st, raw, lt in store_cases:
                label = f"align WriteNewPlace+GetHead {key} ({lt})"
                wd, md = put_and_head(lib_d, hd, key, ref, st, raw, lt)
                wc, mc = put_and_head(lib_c, hc, key, ref, st, raw, lt)
                if wd != wc:
                    print(f"FAIL  {label} (wire differs)")
                    ok = False
                    continue
                if md != mc:
                    print(f"FAIL  {label} (GetHead differs: {md} vs {mc})")
                    ok = False
                    continue
                print(f"PASS  {label}")
        finally:
            lib_d.kvspaceClose(hd)
            lib_c.kvspaceClose(hc)
    return ok


# ── 写侧落盘时机（issue #23）─────────────────────────────────────────
#
# 写把 XValue 字节攒在句柄内（body 由调用方在返回后填，返回前无法落盘）：下一次 op 落盘上一笔
# （读也是 op），另一个句柄随即可见；漏调 Close 而正常退出时由前端退出兜底落盘。子进程必须
# 独立——它正是「不 Close 就退出」的那一个。

CHILD = r'''
import ctypes, sys
U8P = ctypes.POINTER(ctypes.c_uint8)
lib = ctypes.CDLL(sys.argv[1])
lib.kvspaceConnect.restype = ctypes.c_void_p
lib.kvspaceConnect.argtypes = [ctypes.c_char_p]
lib.kvspaceDelTree.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]
lib.kvspaceWriteNewPlace.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint8, ctypes.c_uint8,
    ctypes.c_uint32, ctypes.c_uint64, ctypes.c_char_p,
    ctypes.POINTER(U8P), ctypes.c_char_p, ctypes.c_uint32]
lib.kvspaceGet.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
    ctypes.POINTER(U8P), ctypes.POINTER(ctypes.c_uint32)]
err = ctypes.create_string_buffer(256)

def put(h, key, val):
    body = U8P()
    lt = ("[%d]char/utf8" % len(val)).encode()
    assert lib.kvspaceWriteNewPlace(h, key, 0, 1, len(val), len(val), lt,
                                    ctypes.byref(body), err, 256) == 0, err.value
    ctypes.memmove(body, val, len(val))

def get(h, key):
    out, n = U8P(), ctypes.c_uint32()
    lib.kvspaceGet(h, key, 0, ctypes.byref(out), ctypes.byref(n))
    return None if not out else ctypes.string_at(out, n.value)

pfx = sys.argv[3].encode()
h1 = lib.kvspaceConnect(sys.argv[2].encode())
assert h1, "connect failed"
lib.kvspaceDelTree(h1, pfx + b"/", err, 256)
put(h1, pfx + b"/a", b"va")
put(h1, pfx + b"/b", b"vb")
put(h1, pfx + b"/c", b"vc")
get(h1, pfx + b"/a")               # any op flushes the previous write (c)
h2 = lib.kvspaceConnect(sys.argv[2].encode())
print("next-op-visible", get(h2, pfx + b"/c") is not None)
put(h1, pfx + b"/d", b"vd")        # leave pending write: exit without further ops or Close
sys.exit(0)
'''


def redis_reachable(dsn):
    import socket
    addr = dsn.removeprefix("redis://").split("/", 1)[0]
    host, _, port = addr.partition(":")
    try:
        with socket.create_connection((host or "127.0.0.1", int(port or 6379)), timeout=0.3):
            return True
    except OSError:
        return False


def test_exit_flush():
    """写侧落盘时机：下一次 op 即落盘上一笔；漏调 Close 正常退出不丢最后一笔。"""
    if not Path(FRONTEND_SO).exists():
        print(f'FAIL  exit-flush (缺前端 {FRONTEND_SO})')
        return False
    env = os.environ.copy()
    pfx = f'/flushprobe-{os.getpid()}'
    keys = [f'{pfx}/a', f'{pfx}/b', f'{pfx}/c', f'{pfx}/d']
    with tempfile.TemporaryDirectory(prefix="kvspace-flush-") as tmp:
        cases = [
            ('fs', f'fs://{tmp}/fs'),
            ('shm', f'shm://{tmp}/probe.shm'),
        ]
        rds_dsn = env.get('KVSPACE', 'redis://127.0.0.1:6379')
        if rds_dsn.startswith('redis://') and redis_reachable(rds_dsn):
            cases.insert(0, ('redis', rds_dsn))
        ok = True
        for label, dsn in cases:
            child = subprocess.run([sys.executable, '-c', CHILD, FRONTEND_SO, dsn, pfx],
                                   capture_output=True, text=True, timeout=30, env=env)
            seen = 'next-op-visible True' in child.stdout
            # 子进程已退出（未 Close）：经 CLI 复核四笔全在，最后一笔由退出兜底落盘。
            got = subprocess.run([KVSPACE_BIN, 'get', *keys], capture_output=True, text=True,
                                 timeout=10, env={**env, 'KVSPACE': dsn})
            if label == 'redis':
                subprocess.run([KVSPACE_BIN, 'deltree', f'{pfx}/'], capture_output=True,
                               timeout=10, env={**env, 'KVSPACE': dsn})
            kept = [l for l in got.stdout.splitlines() if 'char/utf8:v' in l]
            if seen and len(kept) == len(keys):
                print(f'PASS  exit-flush {label}')
            else:
                print(f'FAIL  exit-flush {label} (next-op-visible={seen}, 落地 {len(kept)}/{len(keys)})')
                if child.stderr:
                    print(f'  child stderr: {child.stderr.strip()}')
                if got.stderr:
                    print(f'  cli stderr: {got.stderr.strip()}')
                ok = False
        return ok


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    scripts = sorted(
        os.path.join(here, f) for f in os.listdir(here)
        if f.endswith('.sh')
    )
    if not scripts:
        print('no scripts found')
        sys.exit(1)

    results = [test_script(s) for s in scripts]
    results.append(test_kvspace_c_alignment())
    results.append(test_exit_flush())
    passed = sum(results)
    print(f'\n{passed}/{len(results)} passed')
    sys.exit(0 if passed == len(results) else 1)


if __name__ == '__main__':
    main()
