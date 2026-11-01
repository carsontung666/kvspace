#!/bin/bash
# expected:
# === deep tree ===
# /deep/a/b/c/d/e/f	int64:6
# a/	lib	
# /deep/a/b/c	int64:3
# d/	lib	
# c/	lib	
# === deltree mid ===
# /deep/a/b/c/d/e/f	(nil)
# /deep/a/b/c/d/e	(nil)
# /deep/a/b/c	int64:3
# === deltree leaf ===
# /deep/a/b/c	(nil)
# a/	lib	
# /end

set -e
KV="${KVSPACE_BIN:-kvspace}"
$KV deltree /deep/

echo "=== deep tree ==="
$KV mkindex /deep/
$KV mkindex /deep/a/
$KV mkindex /deep/a/b/
$KV mkindex /deep/a/b/c/
$KV mkindex /deep/a/b/c/d/
$KV mkindex /deep/a/b/c/d/e/
$KV set /deep/a/b/c/d/e/f int:6
$KV get /deep/a/b/c/d/e/f
$KV list /deep/
$KV set /deep/a/b/c int:3
$KV get /deep/a/b/c
$KV list /deep/a/b/c/
$KV list /deep/a/b/

echo "=== deltree mid ==="
$KV deltree /deep/a/b/c/d/e/
$KV get /deep/a/b/c/d/e/f
$KV get /deep/a/b/c/d/e
$KV get /deep/a/b/c

echo "=== deltree leaf ==="
$KV deltree /deep/a/b/
$KV get /deep/a/b/c
$KV list /deep/






