#!/bin/bash
# Stage GNU.make + GNU.gcc as Lunar .oaa artifacts (gzip tar for BusyBox tar).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OKRAPM="$(cd "$(dirname "$0")/.." && pwd)"
SYS="${OKRALINUX:-$ROOT/OKRALINUX}"
REPO="${REPO:-$OKRAPM/repo}"
SRC="$OKRAPM/../okra-linux/live-build/src"
GCC_OAA="${GCC_OAA:-$ROOT/gcc-16.2.1.oaa}"
MAKE_VER="${MAKE_VER:-4.4.1}"
JOBS="${JOBS:-$(nproc)}"

mkdir -p "$REPO/artifacts" "$SRC"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

# --- GNU.make (sysroot build against OKRALINUX glibc) ---
MAKE_TARBALL="$SRC/make-${MAKE_VER}.tar.gz"
if [ ! -f "$MAKE_TARBALL" ]; then
    echo "==> downloading GNU make $MAKE_VER"
    curl -L --fail -o "$MAKE_TARBALL" "https://ftp.gnu.org/gnu/make/make-${MAKE_VER}.tar.gz"
fi
MAKE_SRC="$STAGE/make-${MAKE_VER}"
tar -C "$STAGE" -xzf "$MAKE_TARBALL"
MAKE_PREFIX="$STAGE/make-prefix"
mkdir -p "$MAKE_PREFIX"
(
    cd "$MAKE_SRC"
    ./configure \
        CC="gcc --sysroot=$SYS -B$SYS/usr/lib64" \
        --prefix=/usr \
        --disable-nls
    make -j"$JOBS"
    make DESTDIR="$MAKE_PREFIX" install-strip || make DESTDIR="$MAKE_PREFIX" install
)
MAKE_PKG="$STAGE/GNU.make"
mkdir -p "$MAKE_PKG/files"
cp -a "$MAKE_PREFIX/." "$MAKE_PKG/files/"
cat > "$MAKE_PKG/meta.yaml" <<EOF
name: make
namespace: GNU
version: ${MAKE_VER}
description: "GNU Make"
architecture: x86_64
maintainer: "OkraLinux Team <maintainer@okralinux.cn>"
installed_size: $(du -sb "$MAKE_PKG/files" | awk '{print $1}')
dependencies: []
files:
  - /usr/bin/make
EOF
MAKE_OAA="$REPO/artifacts/GNU.make@${MAKE_VER}.oaa"
tar -czf "$MAKE_OAA" -C "$MAKE_PKG" .
sha256sum "$MAKE_OAA" | awk '{print $1}' > "$MAKE_OAA.sha256"
echo "==> $MAKE_OAA"

# --- GNU.gcc (reuse existing toolchain payload, Lunar name GNU.gcc) ---
[ -f "$GCC_OAA" ] || { echo "missing $GCC_OAA"; exit 1; }
GCC_PKG="$STAGE/GNU.gcc"
mkdir -p "$GCC_PKG"
tar -xf "$GCC_OAA" -C "$GCC_PKG"
cat > "$GCC_PKG/meta.yaml" <<'EOF'
name: gcc
namespace: GNU
version: 16.2.1
description: "GNU Compiler Collection (C/C++ frontends and tools)"
architecture: x86_64
maintainer: "OkraLinux Team <maintainer@okralinux.cn>"
installed_size: 220567536
dependencies:
  - GNU.make
files:
  - /usr/bin/gcc
  - /usr/bin/g++
  - /usr/bin/cpp
  - /usr/bin/cc
  - /usr/libexec/gcc
  - /usr/lib/gcc
EOF
GCC_OUT="$REPO/artifacts/GNU.gcc@16.2.1.oaa"
tar -czf "$GCC_OUT" -C "$GCC_PKG" .
sha256sum "$GCC_OUT" | awk '{print $1}' > "$GCC_OUT.sha256"
echo "==> $GCC_OUT"

python3 - <<PY
from pathlib import Path
import sys
sys.path.insert(0, "$OKRAPM/repo-server")
import server
server.write_index(Path("$REPO"))
print("index:", Path("$REPO") / "index.yaml")
PY
ls -lh "$REPO/artifacts"
