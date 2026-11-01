#!/bin/bash
# expected:
# === scalar (ref=0, storetype=0) ===
# /t14/a	int64	ref=0	storetype=0	ndim=0	dims=[]
# === slack string (ref=0, storetype=1) ===
# /t14/c	char/utf8	ref=0	storetype=1	ndim=0	dims=[]
# === typed ptr (ref=1, storetype=1) ===
# /t14/d	int64	ref=1	storetype=1	ndim=0	dims=[]
# === untyped ptr (ref=1, storetype=1) ===
# /t14/e		ref=1	storetype=1	ndim=0	dims=[]
# /end

set -e
KV="${KVSPACE_BIN:-kvspace}"
$KV deltree /t14/

echo "=== scalar (ref=0, storetype=0) ==="
$KV set /t14/a int:42
$KV head /t14/a

echo "=== slack string (ref=0, storetype=1) ==="
$KV set /t14/c string:hello
$KV head /t14/c

echo "=== typed ptr (ref=1, storetype=1) ==="
$KV set /t14/d '*:int64:/t14/a'
$KV head /t14/d

echo "=== untyped ptr (ref=1, storetype=1) ==="
$KV set /t14/e '*:/t14/a'
$KV head /t14/e
