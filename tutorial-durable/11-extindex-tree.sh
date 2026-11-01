#!/bin/bash
# expected:
# === tree showext true ===
# /ext/
# ├── a	1
# ├── x/
# │   └── z	99
# └── z	42
# === tree showext false ===
# /ext/
# ├── x/
# │   └── z	99
# └── z	42
# └── …/base/
# /end

set -e
KV="${KVSPACE_BIN:-kvspace}"
$KV deltree /ext/
$KV deltree /base/

$KV mkindex /base/
$KV set /base/a int:1
$KV extindex /ext/ /base/
$KV set /ext/z int:42
$KV mkindex /ext/x/
$KV set /ext/x/z int:99

echo "=== tree showext true ==="
$KV tree /ext/

echo "=== tree showext false ==="
$KV tree --showext=false /ext/







