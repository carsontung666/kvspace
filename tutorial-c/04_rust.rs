// 04_rust — Rust FFI 直调 libkvspace（dispatch 前端），shm:// 后端
// #571: Fixed 8-byte header (body offset is always 8); langtype in inline attr region.
// 编译: rustc 04_rust.rs -l kvspace

use std::ffi::{c_char, c_int, c_void, CString};
use std::ptr;

// Matches kvspaceHead_t (langtype populated by kvspaceGetHead from inline attr).
#[repr(C)]
struct Head {
    headlen: u16,
    r#ref: u8,
    storetype: u8,
    body_len: i32,
    ndim: i32,
    dims: [i32; 8],
    langtype: [u8; 256],
    langtype_len: i32,
    body_offset: i32,
    body_cap: u64,
}

impl Default for Head {
    fn default() -> Self {
        Head { headlen: 0, r#ref: 0, storetype: 0, body_len: 0, ndim: 0, dims: [0; 8],
               langtype: [0; 256], langtype_len: 0, body_offset: 0, body_cap: 0 }
    }
}

// 固定 8 字节 head：[len:u48 LE@0-5][storetype@6][ptr@7]，body 偏移恒 8。
#[repr(C)]
struct XvalueHead {
    len: [u8; 6],
    storetype: u8,
    ptr: u8,
}

fn xvalue_head_len(h: &XvalueHead) -> u64 {
    let mut v: u64 = 0;
    for i in (0..6).rev() { v = (v << 8) | h.len[i] as u64; }
    v
}

extern "C" {
    fn kvspaceConnect(dsn: *const c_char) -> *mut c_void;
    fn kvspaceClose(h: *mut c_void);
    // codec 产出为 frontend malloc 缓冲，拷出后以 libc free 释放（无 kvspaceBytesFree）。
    fn free(p: *mut c_void);
    // 借用读：out 指向 kvspace 常驻空间，调用方不得 free。resolve=1 穿透 link。
    fn kvspaceGet(h: *mut c_void, key: *const c_char, resolve: c_int,
                  out: *mut *mut u8, out_len: *mut u32) -> c_int;
    fn kvspaceGetHead(h: *mut c_void, key: *const c_char, out: *mut Head) -> c_int;
    // 就地写：key 已存在且 body_len 不变 → 返回原 box body 偏移指针；否则非 0。
    fn kvspaceWriteInPlace(h: *mut c_void, key: *const c_char, resolve: c_int, body_len: u32,
                           body: *mut *mut u8, err: *mut c_char, err_cap: u32) -> c_int;
    // Allocate box and write fixed header + attr region (including langtype), returning body pointer.
    fn kvspaceWriteNewPlace(h: *mut c_void, key: *const c_char, r#ref: u8, storetype: u8,
                            body_len: u32, body_cap: u64, langtype: *const c_char,
                            body: *mut *mut u8, err: *mut c_char, err_cap: u32) -> c_int;
    // 前缀枚举：ListLen 定计数，逐 idx ListAt 借用取名（不得 free）。
    fn kvspaceListLen(h: *mut c_void, prefix: *const c_char, expand_ext: c_int,
                      resolve: c_int, out_count: *mut i32) -> c_int;
    fn kvspaceListAt(h: *mut c_void, prefix: *const c_char, expand_ext: c_int, resolve: c_int,
                     idx: i32, out: *mut u8, out_cap: u32, out_len: *mut u32) -> c_int;
    fn kvspaceDel(h: *mut c_void, keys: *const *const c_char, n: u32, err: *mut c_char, err_cap: u32) -> c_int;
    fn kvspaceDelTree(h: *mut c_void, prefix: *const c_char, err: *mut c_char, err_cap: u32) -> c_int;
    fn kvspaceMkindex(h: *mut c_void, path: *const c_char, capacity: u32, err: *mut c_char, err_cap: u32) -> c_int;
    // New*: langtype is stored in inline attr (empty = untyped).
    fn kvspaceNewInt64(v: i64, langtype: *const c_char, out: *mut *mut u8, out_len: *mut u32) -> c_int;
    fn kvspaceNewChar(bytes: *const u8, len: u32, langtype: *const c_char,
                      out: *mut *mut u8, out_len: *mut u32) -> c_int;
}

fn cs(s: &str) -> CString { CString::new(s).unwrap() }

fn enc_int(v: i64) -> Vec<u8> {
    let mut out: *mut u8 = ptr::null_mut();
    let mut n: u32 = 0;
    let lt = cs("int64");
    assert_eq!(unsafe { kvspaceNewInt64(v, lt.as_ptr(), &mut out, &mut n) }, 0);
    let b = unsafe { std::slice::from_raw_parts(out, n as usize) }.to_vec();
    unsafe { free(out as *mut c_void); }
    b
}

fn new_str(s: &str) -> Vec<u8> {
    let b = s.as_bytes();
    let mut out: *mut u8 = ptr::null_mut();
    let mut n: u32 = 0;
    let lt = cs("char/utf8");
    assert_eq!(unsafe { kvspaceNewChar(b.as_ptr(), b.len() as u32, lt.as_ptr(), &mut out, &mut n) }, 0);
    let r = unsafe { std::slice::from_raw_parts(out, n as usize) }.to_vec();
    unsafe { free(out as *mut c_void); }
    r
}

// Read kind via kvspaceGetHead (from inline attr); body starts at offset 8 with length h.body_len.
fn read_kind(kv: *mut c_void, key: &str, data: &[u8]) -> (String, Vec<u8>) {
    let mut h = Head::default();
    if unsafe { kvspaceGetHead(kv, cs(key).as_ptr(), &mut h) } != 0 {
        return (String::new(), vec![]);
    }
    let klen = h.langtype.iter().position(|&b| b == 0).unwrap_or(256);
    let mut kx = String::from_utf8_lossy(&h.langtype[..klen]).into_owned();
    if kx.starts_with('*') || kx.starts_with('@') { kx.drain(..1); }
    if let Some(i) = kx.find(']') { kx.drain(..=i); }
    let bl = h.body_len.max(0) as usize;
    let end = (8 + bl).min(data.len());
    (kx, data[8..end].to_vec())
}

struct KV { ptr: *mut c_void, path: String }

impl KV {
    fn open(path: &str) -> Self {
        let _ = std::fs::remove_file(path);
        let dsn = CString::new(format!("shm://{}", path)).unwrap();
        let ptr = unsafe { kvspaceConnect(dsn.as_ptr()) };
        assert!(!ptr.is_null());
        KV { ptr, path: path.to_string() }
    }
    // Write into returned body pointer; WriteNewPlace writes langtype with the header/attr region.
    fn set(&self, key: &str, langtype: &str, val: &[u8]) {
        if val.len() < 8 { return; }
        let hd: XvalueHead = unsafe { std::ptr::read(val.as_ptr() as *const XvalueHead) };
        let bl = xvalue_head_len(&hd) as u32;
        let ck = cs(key);
        let lt = cs(langtype);
        let mut dst: *mut u8 = ptr::null_mut();
        let mut err = [0i8; 256];
        unsafe {
            if kvspaceWriteInPlace(self.ptr, ck.as_ptr(), 1, bl, &mut dst, err.as_mut_ptr(), 256) != 0 {
                let rc = kvspaceWriteNewPlace(self.ptr, ck.as_ptr(), hd.ptr, hd.storetype,
                                              bl, bl as u64, lt.as_ptr(), &mut dst,
                                              err.as_mut_ptr(), 256);
                assert_eq!(rc, 0, "write {key} failed");
            }
            if bl > 0 && !dst.is_null() {
                ptr::copy_nonoverlapping(val[8..].as_ptr(), dst, bl as usize);
            }
        }
    }
    // 借用读：out 指向 kvspace 常驻空间，拷出自持后即失效，不 free。
    fn get(&self, key: &str) -> Option<Vec<u8>> {
        let ck = CString::new(key).unwrap();
        let mut out: *mut u8 = ptr::null_mut();
        let mut n: u32 = 0;
        unsafe { kvspaceGet(self.ptr, ck.as_ptr(), 0, &mut out, &mut n); }
        if out.is_null() || n == 0 { return None; }
        Some(unsafe { std::slice::from_raw_parts(out, n as usize) }.to_vec())
    }
    fn mkindex(&self, path: &str) {
        let cp = CString::new(path).unwrap();
        unsafe { kvspaceMkindex(self.ptr, cp.as_ptr(), 0, ptr::null_mut(), 0); }
    }
    fn del(&self, key: &str) {
        let ck = CString::new(key).unwrap();
        let keys = [ck.as_ptr()];
        unsafe { kvspaceDel(self.ptr, keys.as_ptr(), 1, ptr::null_mut(), 0); }
    }
    fn deltree(&self, prefix: &str) {
        let cp = CString::new(prefix).unwrap();
        unsafe { kvspaceDelTree(self.ptr, cp.as_ptr(), ptr::null_mut(), 0); }
    }
    // 前缀枚举：ListLen 定计数 + 逐 idx ListAt 借用取名（读出即拷入 Vec）。
    fn list(&self, prefix: &str) -> Vec<String> {
        let cp = CString::new(prefix).unwrap();
        let mut count: i32 = 0;
        if unsafe { kvspaceListLen(self.ptr, cp.as_ptr(), 0, 1, &mut count) } != 0 || count <= 0 {
            return vec![];
        }
        let mut r = Vec::with_capacity(count as usize);
        for i in 0..count {
            let mut nm = [0u8; 1024];
            let mut n: u32 = 0;
            unsafe { kvspaceListAt(self.ptr, cp.as_ptr(), 0, 1, i, nm.as_mut_ptr(), nm.len() as u32, &mut n); }
            if n > 0 {
                r.push(String::from_utf8_lossy(&nm[..n as usize]).into_owned());
            }
        }
        r
    }
}

impl Drop for KV {
    fn drop(&mut self) {
        unsafe { kvspaceClose(self.ptr); }
        let _ = std::fs::remove_file(&self.path);
    }
}

fn main() {
    let kv = KV::open("/tmp/kvspace_rs.shm");
    kv.mkindex("/rs/");
    kv.set("/rs/a", "int64", &enc_int(42));
    kv.set("/rs/b", "char/utf8", &new_str("hello"));

    let v = kv.get("/rs/a").unwrap();
    let (kind, raw) = read_kind(kv.ptr, "/rs/a", &v);
    let mut arr = [0u8; 8];
    arr.copy_from_slice(&raw[..8]);
    let val = i64::from_le_bytes(arr);
    println!("/rs/a  kind={kind}  val={val}");

    let ns = kv.list("/rs/");
    println!("list /rs/: {ns:?} (count={})", ns.len());

    kv.deltree("/rs/");
    println!("PASS rust");
}
