// kvspace CLI — 走 libkvspace dispatch 前端（C ABI），后端由 DSN 运行时选择。
// 命令与输出格式对齐 kvspace-go/cmd/kvspace。

use std::env;
use std::os::raw::{c_char, c_int, c_void};
use std::process::exit;

/* 固定 head 8 字节（见 kvspace/xvalue_head.h）：body 偏移恒 8。 */
const KVSPACE_XVALUE_HEAD_LEN: usize = 8;
const KVSPACE_XVALUE_ST_SHORT: u8 = 0;
/* head.ptr: 0=inline, 1=ptr, 2=@ext (KVSPACE_XVALUE_PTR_*). */
const KVSPACE_XVALUE_PTR_PTR: u8 = 1;
const KVSPACE_XVALUE_PTR_EXT: u8 = 2;

#[link(name = "kvspace")]
extern "C" {
    fn kvspaceConnect(dsn: *const c_char) -> *mut c_void;
    fn kvspaceClose(h: *mut c_void);
    fn free(p: *mut c_void); // libc：释放 codec 产出的 frontend malloc 缓冲
                             // 借用读：*out 指向后端常驻/回收空间，调用方不得 free。resolve=1 穿透 link。
    fn kvspaceGet(
        h: *mut c_void,
        key: *const c_char,
        resolve: c_int,
        out: *mut *mut u8,
        out_len: *mut u32,
    ) -> c_int;
    fn kvspaceSetValue(
        h: *mut c_void,
        key: *const c_char,
        value: *const u8,
        value_len: u32,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    // 前缀遍历：listlen 定计数，逐 idx 取名（借用回收缓冲，不得 free），不一次性返回整段名单。
    fn kvspaceListLen(
        h: *mut c_void,
        prefix: *const c_char,
        expand_ext: c_int,
        resolve: c_int,
        out_count: *mut i32,
    ) -> c_int;
    fn kvspaceListAt(
        h: *mut c_void,
        prefix: *const c_char,
        expand_ext: c_int,
        resolve: c_int,
        idx: i32,
        buf: *mut u8,
        buf_cap: u32,
        out_len: *mut u32,
    ) -> c_int;
    fn kvspaceDel(
        h: *mut c_void,
        keys: *const *const c_char,
        nkeys: u32,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceDelTree(
        h: *mut c_void,
        prefix: *const c_char,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceCp(
        h: *mut c_void,
        src: *const c_char,
        dst: *const c_char,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceCpTree(
        h: *mut c_void,
        src: *const c_char,
        dst: *const c_char,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceMkindex(
        h: *mut c_void,
        path: *const c_char,
        capacity: u32,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceMkindexExt(
        h: *mut c_void,
        path: *const c_char,
        ext: *const c_char,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceRmindexExt(
        h: *mut c_void,
        path: *const c_char,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceClear(h: *mut c_void, err: *mut c_char, err_cap: u32) -> c_int;
    fn kvspaceNewPtr(
        target_kindexpr: *const c_char,
        target: *const c_char,
        out: *mut *mut u8,
        out_len: *mut u32,
    ) -> c_int;
    fn kvspaceNewChar(
        bytes: *const u8,
        len: u32,
        langtype: *const c_char,
        out: *mut *mut u8,
        out_len: *mut u32,
    ) -> c_int;
    fn kvspaceNewBool(
        v: u8,
        langtype: *const c_char,
        out: *mut *mut u8,
        out_len: *mut u32,
    ) -> c_int;
    fn kvspaceNewInt64(
        v: i64,
        langtype: *const c_char,
        out: *mut *mut u8,
        out_len: *mut u32,
    ) -> c_int;
    fn kvspaceNewFloat64(
        v: f64,
        langtype: *const c_char,
        out: *mut *mut u8,
        out_len: *mut u32,
    ) -> c_int;
    fn kvspaceXvalueHeadNewScalar(
        storetype: u8,
        raw: *const u8,
        raw_len: u32,
        langtype: *const c_char,
        out: *mut *mut u8,
        out_len: *mut u64,
    ) -> c_int;
}

fn cs(s: &str) -> *const c_char {
    // &str 不保证 NUL 结尾（尤其字符串字面量），必须经 CString 补 NUL。
    // CLI 短生命周期，泄漏即可（进程退出统一回收）。
    std::ffi::CString::new(s).unwrap().into_raw() as *const c_char
}

fn default_dsn() -> String {
    env::var("KVSPACE").unwrap_or_else(|_| "redis://127.0.0.1:6379".to_string())
}

fn fatalf(msg: &str) -> ! {
    eprintln!("{}", msg);
    exit(1)
}

// 后端可能成功也可能失败但不填 err（如 shm 若干写路径）：空 err 给兜底，绝不静默当成功。
fn fail_op(err: &[u8; 256], op: &str) -> ! {
    let n = err.iter().position(|&b| b == 0).unwrap_or(err.len());
    if n > 0 {
        fatalf(&String::from_utf8_lossy(&err[..n]));
    }
    fatalf(&format!("{}: failed（后端未填错误详情）", op));
}

struct Value {
    kind: String,   // Base kind (langtype without leading [dims])
    r#ref: u8,      // head.ptr: 0=inline, 1=ptr, 2=@ext
    storetype: u8,  // head.storetype
    dims: Vec<i32>, // Leading [dims] parsed from langtype
    body: Vec<u8>,
}

/* head 固定 8 字节 [len:u48 LE@0-5][storetype@6][ptr@7]，body 偏移恒 8；
   langtype 作为 attr 记录 tag=1 置于 body 之尾（值内，非独立 key，见 #571）。
   值自描述：直接解 wire，不经 kvspaceGetHead（它对 Ptr 穿透，读不到本体）。 */
fn from_wire(data: &[u8]) -> Value {
    if data.len() < KVSPACE_XVALUE_HEAD_LEN {
        return Value {
            kind: String::new(),
            r#ref: 0,
            storetype: 0,
            dims: vec![],
            body: vec![],
        };
    }
    let body_len = wire_len(data) as usize;
    let (dims, kind) = parse_langtype(&wire_langtype(data, body_len));
    /* body 只到 head.len 为止；其后是 attr 区，不属 body。 */
    let start = KVSPACE_XVALUE_HEAD_LEN;
    let end = start.saturating_add(body_len).min(data.len());
    Value {
        kind,
        r#ref: data[7],
        storetype: data[6],
        dims,
        body: data[start..end].to_vec(),
    }
}

/// u48 小端 @0-5（唯一读点，与 C 侧 kvspaceXvalueHeadLen 一致）。
fn wire_len(data: &[u8]) -> u64 {
    let mut v = 0u64;
    for i in (0..6).rev() {
        v = (v << 8) | data[i] as u64;
    }
    v
}

/// 扫 attr 区取 tag=1（langtype）；无记录/坏 attr → 空串。
fn wire_langtype(data: &[u8], body_len: usize) -> String {
    let base = KVSPACE_XVALUE_HEAD_LEN.saturating_add(body_len);
    if base.saturating_add(2) > data.len() {
        return String::new();
    }
    let alen = (data[base] as usize) | ((data[base + 1] as usize) << 8);
    if alen < 2 || base + alen > data.len() {
        return String::new();
    }
    let end = base + alen;
    let mut p = base + 2;
    while p + 3 <= end {
        let tag = data[p];
        let rlen = (data[p + 1] as usize) | ((data[p + 2] as usize) << 8);
        if p + 3 + rlen > end {
            return String::new();
        }
        if tag == 1 {
            return String::from_utf8_lossy(&data[p + 3..p + 3 + rlen]).into_owned();
        }
        p += 3 + rlen;
    }
    String::new()
}

/// langtype 无 ref 前缀（ref 是独立字段）：只剥前导 [dims]，其余为基 kind。
fn parse_langtype(lt: &str) -> (Vec<i32>, String) {
    if lt.contains('·') {
        return (Vec::new(), lt.to_string());
    }
    if let Some(rest) = lt.strip_prefix('[') {
        if let Some(end) = rest.find(']') {
            let dims: Vec<i32> = rest[..end]
                .split(',')
                .filter(|s| !s.is_empty())
                .map(|s| s.parse().unwrap_or(0))
                .collect();
            return (dims, rest[end + 1..].to_string());
        }
    }
    (Vec::new(), lt.to_string())
}

fn fmt_float(v: f64) -> String {
    let s = format!("{}", v);
    if s.contains('.') {
        s
    } else {
        format!("{}.0", s)
    }
}

/// Element width in bytes for fixed-width scalars; None otherwise.
fn scalar_width(kind: &str) -> Option<usize> {
    match kind {
        "bool" | "int8" | "uint8" => Some(1),
        "int16" | "uint16" => Some(2),
        "int32" | "uint32" | "float32" => Some(4),
        "int64" | "uint64" | "float64" => Some(8),
        _ => None,
    }
}

/// Decode one little-endian fixed-width scalar; returns None on unknown kind or short slice.
fn scalar_text(kind: &str, b: &[u8]) -> Option<String> {
    let w = scalar_width(kind)?;
    let b = b.get(..w)?;
    Some(match kind {
        "bool" => (b[0] != 0).to_string(),
        "int8" => (b[0] as i8).to_string(),
        "uint8" => b[0].to_string(),
        "int16" => i16::from_le_bytes(b.try_into().ok()?).to_string(),
        "uint16" => u16::from_le_bytes(b.try_into().ok()?).to_string(),
        "int32" => i32::from_le_bytes(b.try_into().ok()?).to_string(),
        "uint32" => u32::from_le_bytes(b.try_into().ok()?).to_string(),
        "float32" => fmt_float(f32::from_le_bytes(b.try_into().ok()?) as f64),
        "int64" => i64::from_le_bytes(b.try_into().ok()?).to_string(),
        "uint64" => u64::from_le_bytes(b.try_into().ok()?).to_string(),
        "float64" => fmt_float(f64::from_le_bytes(b.try_into().ok()?)),
        _ => return None,
    })
}

fn join_dims(dims: &[i32]) -> String {
    dims.iter()
        .map(|d| d.to_string())
        .collect::<Vec<_>>()
        .join(",")
}

fn plain(v: &Value) -> String {
    // ref 先于 kind 判空：不约束指针（空 kindexpr）也是合法值。
    if v.r#ref == KVSPACE_XVALUE_PTR_PTR {
        return format!("→{}", String::from_utf8_lossy(&v.body));
    }
    if v.r#ref == KVSPACE_XVALUE_PTR_EXT {
        return format!("@{}", String::from_utf8_lossy(&v.body));
    }
    if v.kind.is_empty() {
        return "None".to_string();
    }
    if let Some(w) = scalar_width(&v.kind) {
        // Format fixed-width numeric tensors ([dims]) element-by-element.
        if !v.dims.is_empty() {
            let items: Vec<String> = v
                .body
                .chunks_exact(w)
                .filter_map(|c| scalar_text(&v.kind, c))
                .collect();
            return format!("[{}]", items.join(","));
        }
        return scalar_text(&v.kind, &v.body)
            .unwrap_or_else(|| format!("<short body: {} bytes>", v.body.len()));
    }
    match v.kind.as_str() {
        "char/utf32" => v
            .body
            .chunks_exact(4)
            .map(|c| {
                char::from_u32(u32::from_le_bytes([c[0], c[1], c[2], c[3]])).unwrap_or('\u{FFFD}')
            })
            .collect(),
        "stringkeymap" => format!("map[{}]", join_dims(&v.dims)),
        "index" => format!("({})", v.dims.first().copied().unwrap_or(0)),
        // char/utf8, char/ascii, and other kinds: decode as lossy UTF-8.
        _ => String::from_utf8_lossy(&v.body).into_owned(),
    }
}

fn format_value(v: &Value) -> String {
    if v.r#ref == KVSPACE_XVALUE_PTR_PTR {
        return format!("→{}:{}", String::from_utf8_lossy(&v.body), v.kind);
    }
    if v.kind.is_empty() {
        return plain(v); // None or @ext locator
    }
    format!("{}:{}", v.kind, plain(v))
}

// 借用读：kvspaceGet 返回后端常驻/回收空间偏移指针，读出即拷贝自持，绝不 free。resolve=0 不穿透 link。
fn get(kv: *mut c_void, key: &str) -> Vec<u8> {
    let mut out: *mut u8 = std::ptr::null_mut();
    let mut len: u32 = 0;
    unsafe {
        if kvspaceGet(kv, cs(key), 0, &mut out, &mut len) != 0 || out.is_null() || len == 0 {
            return vec![];
        }
        std::slice::from_raw_parts(out, len as usize).to_vec()
    }
}

// Parse CLI value syntax into full XValue wire bytes (langtype stored in tag=1 attr).
// Empty Vec represents `nil:` (delete).
fn parse_value(raw: &str) -> Vec<u8> {
    let mut out: *mut u8 = std::ptr::null_mut();
    let mut len: u32 = 0;
    if let Some(rest) = raw.strip_prefix('*') {
        // `*:kindexpr:target` (typed ptr) or `*:target` (unconstrained ptr).
        let rest = rest.strip_prefix(':').unwrap_or(rest);
        let (k, t) = match rest.find(':') {
            Some(i) => (&rest[..i], &rest[i + 1..]),
            None => ("", rest),
        };
        if unsafe { kvspaceNewPtr(cs(k), cs(t), &mut out, &mut len) != 0 } {
            fatalf(&format!("parse value failed: {:?}", raw));
        }
        // Unprefixed kindexpr; empty kindexpr -> unconstrained (empty langtype).
        return take_wire(out, len);
    }
    if let Some(langtype) = raw.strip_prefix("map:") {
        if !langtype.contains('·') {
            fatalf("map requires key·value langtype");
        }
        // Empty-body short header with langtype in the attr region.
        let mut len64: u64 = 0;
        if unsafe {
            kvspaceXvalueHeadNewScalar(
                KVSPACE_XVALUE_ST_SHORT,
                std::ptr::null(),
                0,
                cs(langtype),
                &mut out,
                &mut len64,
            ) != 0
        } {
            fatalf(&format!("parse value failed: {:?}", raw));
        }
        return take_wire(out, len64 as u32); // bare kindexpr (e.g. int·string)
    }
    let split = raw.find(':').unwrap_or(raw.len());
    let kind = &raw[..split];
    let repr = raw[split..].trim_start_matches(':');
    let langtype = match kind {
        "int" => "int64",
        "float" => "float64",
        "float32" => "float32",
        "bool" => "bool",
        _ => "[]char/utf8", // string 与默认字符串
    };
    match kind {
        "int" => unsafe {
            if kvspaceNewInt64(
                repr.parse().unwrap_or_else(|_| fatalf(&format!("parse value failed: {:?}", raw))),
                cs(langtype),
                &mut out,
                &mut len,
            ) != 0
            {
                fatalf(&format!("parse value failed: {:?}", raw));
            }
        },
        "float" => unsafe {
            if kvspaceNewFloat64(
                repr.parse().unwrap_or_else(|_| fatalf(&format!("parse value failed: {:?}", raw))),
                cs(langtype),
                &mut out,
                &mut len,
            ) != 0
            {
                fatalf(&format!("parse value failed: {:?}", raw));
            }
        },
        "float32" => {
            let f: f32 = repr
                .parse()
                .unwrap_or_else(|_| fatalf(&format!("parse value failed: {:?}", raw)));
            // 4 字节小端 f32 body 的短定长头。
            let le = f.to_le_bytes();
            let mut len64: u64 = 0;
            if unsafe {
                kvspaceXvalueHeadNewScalar(
                    KVSPACE_XVALUE_ST_SHORT,
                    le.as_ptr(),
                    4,
                    cs(langtype),
                    &mut out,
                    &mut len64,
                ) != 0
            } {
                fatalf(&format!("parse value failed: {:?}", raw));
            }
            len = len64 as u32;
        }
        "bool" => {
            let b = match repr {
                "true" => true,
                "false" => false,
                _ => fatalf(&format!("bool 只接受 true/false: {:?}", raw)),
            };
            unsafe {
                if kvspaceNewBool(b as u8, cs(langtype), &mut out, &mut len) != 0 {
                    fatalf(&format!("parse value failed: {:?}", raw));
                }
            }
        }
        "string" => unsafe {
            if kvspaceNewChar(
                repr.as_ptr(),
                repr.len() as u32,
                cs(langtype),
                &mut out,
                &mut len,
            ) != 0
            {
                fatalf(&format!("parse value failed: {:?}", raw));
            }
        },
        "nil" => return Vec::new(), // delete (no value)
        "index" => fatalf("use mkindex for directories"),
        // Default to string when no recognized kind prefix is present.
        _ => unsafe {
            if kvspaceNewChar(
                raw.as_ptr(),
                raw.len() as u32,
                cs(langtype),
                &mut out,
                &mut len,
            ) != 0
            {
                fatalf(&format!("parse value failed: {:?}", raw));
            }
        },
    }
    take_wire(out, len)
}

// codec 产出为 frontend malloc，读出即 free；失败已 fatalf 退出，到这里的 out 恒非空。
fn take_wire(out: *mut u8, len: u32) -> Vec<u8> {
    unsafe {
        let v = std::slice::from_raw_parts(out, len as usize).to_vec();
        free(out as *mut c_void); // codec 产出为 frontend malloc，libc free
        v
    }
}

// Single-key full-value write (head + body + attr tail).
fn set_one(kv: *mut c_void, key: &str, val: &[u8]) {
    if val.is_empty() {
        // `nil:` deletes the key; report errors just like `del` (missing key returns 0).
        let key_ptr = cs(key);
        let mut err = [0u8; 256];
        let rc = unsafe { kvspaceDel(kv, &key_ptr, 1, err.as_mut_ptr() as *mut c_char, 256) };
        if rc != 0 {
            fail_op(&err, &format!("set {} nil", key));
        }
        return;
    }
    let ck = cs(key);
    let mut err = [0u8; 256];
    let rc = unsafe {
        kvspaceSetValue(
            kv,
            ck,
            val.as_ptr(),
            val.len() as u32,
            err.as_mut_ptr() as *mut c_char,
            256,
        )
    };
    if rc != 0 {
        let n = err.iter().position(|&b| b == 0).unwrap_or(err.len());
        fatalf(&format!(
            "set {}: {}",
            key,
            String::from_utf8_lossy(&err[..n])
        ));
    }
}

// Normalize directory prefix (trailing `/` or `·`): ListLen/ListAt, mkindex, and
// extindex mounts address directory keys; append `/` to bare prefixes like `/t`.
fn dir_form(prefix: &str) -> String {
    if prefix.ends_with('/') || prefix.ends_with('·') {
        prefix.to_string()
    } else {
        format!("{}/", prefix)
    }
}

// Prefix listing via ListLen + ListAt (resolve=1 follows links; expand_ext=false lists own children only).
fn list_names(kv: *mut c_void, prefix: &str, expand_ext: bool) -> Vec<String> {
    let p = cs(&dir_form(prefix));
    let mut count: i32 = 0;
    unsafe {
        if kvspaceListLen(kv, p, expand_ext as c_int, 1, &mut count) != 0 || count <= 0 {
            return vec![];
        }
        let mut v = Vec::with_capacity(count as usize);
        let mut buf = [0u8; 4096];
        for i in 0..count {
            let mut len: u32 = 0;
            if kvspaceListAt(
                kv,
                p,
                expand_ext as c_int,
                1,
                i,
                buf.as_mut_ptr(),
                buf.len() as u32,
                &mut len,
            ) == 0
                && len > 0
            {
                let name = String::from_utf8_lossy(&buf[..len as usize]).into_owned();
                if !name.is_empty() {
                    v.push(name);
                }
            }
        }
        v
    }
}

fn join_path(parent: &str, child: &str) -> String {
    if parent == "/" {
        return format!("/{}", child);
    }
    if parent.ends_with('/') || parent.ends_with('·') {
        return format!("{}{}", parent, child);
    }
    format!("{}/{}", parent, child)
}

// An extindex mount is an @ext value on the directory key (head.ptr=2), whose body
// holds the source path. Ignore langtype since backends differ on whether it is set.
fn read_ext(kv: *mut c_void, prefix: &str) -> String {
    let v = get(kv, &dir_form(prefix));
    if v.is_empty() {
        return String::new();
    }
    let d = from_wire(&v);
    if d.r#ref != KVSPACE_XVALUE_PTR_EXT {
        return String::new();
    }
    String::from_utf8_lossy(&d.body).into_owned()
}

// Shared list/tree arguments: [prefix] [--kind[=false]] [--showext[=false]]; default prefix is `/`.
fn parse_walk_args(tail: &[String], show_kind_default: bool) -> (String, bool, bool) {
    let mut show_ext = true;
    let mut show_kind = show_kind_default;
    let mut prefix = "/".to_string();
    for a in tail {
        match a.as_str() {
            "--kind" => show_kind = true,
            "--kind=false" => show_kind = false,
            "--showext" => show_ext = true,
            "--showext=false" => show_ext = false,
            _ => prefix = a.clone(),
        }
    }
    (prefix, show_ext, show_kind)
}

fn print_help() {
    println!("usage: kvspace <subcommand> [args]   (后端由 env KVSPACE 的 DSN 选择)");
    println!();
    println!("子命令:");
    println!("  get <key>...            读取并打印 key:kind:value");
    println!("  set <key> <value>       写入值；value 语法见下；nil: 删除键");
    println!("  head <key>...           打印值头（ref/storetype/ndim/dims）");
    println!("  del <key>...            删除键");
    println!("  deltree <prefix>        删除整棵子树");
    println!("  cp <src> <dst>          拷贝单键");
    println!("  cpdir <src> <dst>       递归拷贝子树");
    println!("  mkindex <path> [capacity]  创建目录（index）");
    println!("  extindex <path> <src_key> 挂载扩展存储（类型取 src_key 的 langtype；path/src_key 须目录形键）");
    println!("  delextindex <path>      卸载扩展存储");
    println!("  list|ls <prefix> [--kind[=false]] [--showext[=false]]  列子键");
    println!("  tree <prefix> [--kind] [--showext=false]  树状遍历");
    println!("  clear                   清空 kvspace");
    println!("  help                    print this help");
    println!();
    println!("value 语法: [kind:]payload");
    println!("  int:42  float:3.14  float32:1.5  bool:true|false  string:foo");
    println!("  map:key·value  空 map（如 map:int·string）");
    println!("  *:kindexpr:target  指针（如 *:int64:/x）；*:target 为不约束指针");
    println!("  无 kind 前缀或未识别前缀 → 默认字符串");
}

fn main() {
    let rest: Vec<String> = env::args().skip(1).collect();
    let dsn = default_dsn();
    if rest.is_empty() {
        eprintln!("usage: kvspace <subcommand> [args]   (后端由 env KVSPACE 的 DSN 选择)");
        eprintln!("       kvspace help  查看子命令清单");
        exit(1);
    }
    let sub = rest[0].clone();
    if matches!(sub.as_str(), "help" | "--help" | "-h") {
        print_help();
        return; // help 不需要连接
    }

    let kv = unsafe { kvspaceConnect(cs(&dsn)) };
    if kv.is_null() {
        fatalf(&format!("connect failed: {dsn}"));
    }

    let tail = &rest[1..];
    match sub.as_str() {
        "get" => {
            if tail.is_empty() {
                fatalf("usage: kvspace get <key>...");
            }
            for k in tail {
                let v = get(kv, k);
                if v.is_empty() {
                    println!("{}\t(nil)", k);
                } else {
                    println!("{}\t{}", k, format_value(&from_wire(&v)));
                }
            }
        }
        "set" => {
            if tail.len() != 2 {
                fatalf("usage: kvspace set <key> <value>");
            }
            set_one(kv, &tail[0], &parse_value(&tail[1]));
        }
        "head" => {
            if tail.is_empty() {
                fatalf("usage: kvspace head <key>...");
            }
            for k in tail {
                let v = get(kv, k);
                if v.is_empty() {
                    println!("{}\t(nil)", k);
                } else {
                    // ndim/dims come from leading [dims] in langtype.
                    let d = from_wire(&v);
                    println!(
                        "{}\t{}\tref={}\tstoretype={}\tndim={}\tdims=[{}]",
                        k,
                        d.kind,
                        d.r#ref,
                        d.storetype,
                        d.dims.len(),
                        join_dims(&d.dims)
                    );
                }
            }
        }
        "del" => {
            if tail.is_empty() {
                fatalf("usage: kvspace del <key>...");
            }
            let keys: Vec<*const c_char> = tail.iter().map(|k| cs(k)).collect();
            let mut err = [0u8; 256];
            let rc = unsafe {
                kvspaceDel(
                    kv,
                    keys.as_ptr(),
                    keys.len() as u32,
                    err.as_mut_ptr() as *mut c_char,
                    256,
                )
            };
            if rc != 0 {
                fail_op(&err, "del");
            }
        }
        "deltree" => {
            let p = tail
                .first()
                .unwrap_or_else(|| fatalf("usage: kvspace deltree <prefix>"));
            let mut err = [0u8; 256];
            let rc = unsafe { kvspaceDelTree(kv, cs(p), err.as_mut_ptr() as *mut c_char, 256) };
            if rc != 0 {
                fail_op(&err, "deltree");
            }
        }
        "cp" => {
            if tail.len() < 2 {
                fatalf("usage: kvspace cp <src> <dst>");
            }
            let mut err = [0u8; 256];
            let rc = unsafe {
                kvspaceCp(
                    kv,
                    cs(&tail[0]),
                    cs(&tail[1]),
                    err.as_mut_ptr() as *mut c_char,
                    256,
                )
            };
            if rc != 0 {
                fail_op(&err, "cp");
            }
        }
        "cpdir" => {
            if tail.len() < 2 {
                fatalf("usage: kvspace cpdir <src> <dst>");
            }
            let mut err = [0u8; 256];
            let rc = unsafe {
                kvspaceCpTree(
                    kv,
                    cs(&tail[0]),
                    cs(&tail[1]),
                    err.as_mut_ptr() as *mut c_char,
                    256,
                )
            };
            if rc != 0 {
                fail_op(&err, "cpdir");
            }
        }
        "mkindex" => {
            let p = tail
                .first()
                .unwrap_or_else(|| fatalf("usage: kvspace mkindex <path> [capacity]"));
            // 目录形归一：shm 自动补 /，durable 强校验 —— CLI 统一补，三后端一致。
            let p = dir_form(p);
            let capacity: u32 = tail.get(1).and_then(|s| s.parse().ok()).unwrap_or(0);
            let mut err = [0u8; 256];
            let rc = unsafe {
                kvspaceMkindex(kv, cs(&p), capacity, err.as_mut_ptr() as *mut c_char, 256)
            };
            if rc != 0 {
                fail_op(&err, "mkindex");
            }
        }
        "delextindex" => {
            let p = tail
                .first()
                .unwrap_or_else(|| fatalf("usage: kvspace delextindex <path>"));
            let mut err = [0u8; 256];
            let rc = unsafe { kvspaceRmindexExt(kv, cs(p), err.as_mut_ptr() as *mut c_char, 256) };
            if rc != 0 {
                fail_op(&err, "delextindex");
            }
        }
        "extindex" => {
            if tail.len() < 2 {
                fatalf("usage: kvspace extindex <path> <src_key>");
            }
            let mut err = [0u8; 256];
            let rc = unsafe {
                kvspaceMkindexExt(
                    kv,
                    cs(&tail[0]),
                    cs(&tail[1]),
                    err.as_mut_ptr() as *mut c_char,
                    256,
                )
            };
            if rc != 0 {
                // Fallback diagnostic when backend leaves err empty.
                fail_op(
                    &err,
                    &format!(
                        "extindex {} {} (path/src_key must be directory keys and src_key must exist)",
                        tail[0], tail[1]
                    ),
                );
            }
        }
        "list" | "ls" => {
            let (p, show_ext, show_kind) = parse_walk_args(tail, true);
            fprint_list(kv, &p, show_ext, show_kind);
        }
        "tree" => {
            let (p, show_ext, show_kind) = parse_walk_args(tail, false);
            println!("{}", p);
            fprint_tree(kv, &p, "", show_ext, show_kind);
        }
        "clear" => {
            let mut err = [0u8; 256];
            let rc = unsafe { kvspaceClear(kv, err.as_mut_ptr() as *mut c_char, 256) };
            if rc != 0 {
                fail_op(&err, "clear");
            }
        }
        other => {
            eprintln!("unknown subcommand: {}", other);
            exit(1);
        }
    }

    unsafe {
        kvspaceClose(kv);
    }
}

fn fprint_list(kv: *mut c_void, prefix: &str, show_ext: bool, show_kind: bool) {
    // When show_ext is false, list own children only and print the ext mount separately below.
    for c in list_names(kv, prefix, show_ext) {
        let v = get(kv, &join_path(prefix, &c));
        if v.is_empty() {
            println!("{}", c);
            continue;
        }
        let d = from_wire(&v);
        if show_kind {
            println!("{}\t{}\t{}", c, d.kind, plain(&d));
        } else {
            println!("{}\t{}", c, plain(&d));
        }
    }
    if !show_ext {
        let ext = read_ext(kv, prefix);
        if !ext.is_empty() {
            println!("…{}", ext);
            for c in list_names(kv, &ext, false) {
                println!("  {}", c);
            }
        }
    }
}

fn fprint_tree(kv: *mut c_void, prefix: &str, indent: &str, show_ext: bool, show_kind: bool) {
    // When show_ext is false, list own children only and print the ext mount separately below.
    let mut children = list_names(kv, prefix, show_ext);
    children.sort_by(|a, b| {
        let (aa, bb) = (a.trim_end_matches('/'), b.trim_end_matches('/'));
        if aa == bb {
            a.ends_with('/').cmp(&b.ends_with('/'))
        } else {
            aa.cmp(bb)
        }
    });

    let n = children.len();
    for (i, c) in children.iter().enumerate() {
        let full = join_path(prefix, c);
        let last = i == n - 1;
        let branch = if last { "└── " } else { "├── " };
        let next_indent = format!("{}{}", indent, if last { "    " } else { "│   " });
        if c.ends_with('/') {
            println!("{}{}{}/", indent, branch, c.trim_end_matches('/'));
            fprint_tree(kv, &full, &next_indent, show_ext, show_kind);
        } else {
            let v = get(kv, &full);
            if v.is_empty() {
                println!("{}{}{}", indent, branch, c);
                continue;
            }
            let d = from_wire(&v);
            if show_kind {
                println!("{}{}{}\t{}\t{}", indent, branch, c, d.kind, plain(&d));
            } else {
                println!("{}{}{}\t{}", indent, branch, c, plain(&d));
            }
        }
    }
    if !show_ext {
        let ext = read_ext(kv, prefix);
        if !ext.is_empty() {
            println!("{}└── …{}", indent, ext);
        }
    }
}
