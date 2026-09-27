#!/bin/sh
# 32-bit sourcevr.so for the native Linux Half-Life 2 (hl2_linux is 32-bit;
# this SDK ships only 64-bit libraries, so VPC cannot build it). Compiles
# this folder's module and the SDK's tier1 inside Valve's Steam Runtime
# "sniper" SDK image, with the SDK project's defines plus NO_MALLOC_OVERRIDE
# (HL2's 32-bit tier0 exports no g_pMemAlloc; the module allocates with
# malloc), and links against the game's own tier0 and vstdlib.
#
# Usage: build32-hl2.sh "<Half-Life 2>/bin" <output-dir>
# Needs docker (or podman with a docker alias) and network for the image.
set -e
HL2BIN=${1:?path to Half-Life 2/bin}
OUT=${2:?output directory}
SRC=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$SRC":/src:ro -v "$HL2BIN":/hl2bin:ro -v "$OUT":/out \
  registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest sh -ec '
  cd /src
  D="-DVPC -DSOURCESDK -DNDEBUG -DGNUC -DPOSIX -D_POSIX -DCOMPILER_GCC -D_DLL_EXT=.so -D_DLL_PREFIX=lib -D_EXTERNAL_DLL_EXT=.so -D_LINUX -DLINUX -DDLLNAME=sourcevr -DUSE_SDL -DNO_MALLOC_OVERRIDE"
  I="-Icommon -Ipublic -Ipublic/tier0 -Ipublic/tier1 -Ipublic/sourcevr"
  F="-m32 -O2 -fPIC -fvisibility=hidden -std=gnu++17 -Wno-register -Wno-deprecated -msse2 -mfpmath=sse"
  mkdir -p /tmp/o
  for f in sourcevr_display/sourcevr_display.cpp \
           tier1/convar.cpp tier1/tier1.cpp tier1/strtools.cpp tier1/characterset.cpp \
           tier1/utlbuffer.cpp tier1/generichash.cpp tier1/strtools_unicode.cpp tier1/utlstring.cpp tier1/qsort_s.cpp; do
    [ -f "$f" ] || { echo "skip $f"; continue; }
    g++ $F $D $I -c "$f" -o /tmp/o/$(basename "$f" .cpp).o
  done
  g++ -m32 -shared -o /out/sourcevr.so /tmp/o/*.o -L/hl2bin -ltier0 -lvstdlib \
      -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -ldl -lm -Wl,--no-undefined
'
cp "$(dirname "$0")/svrtv-anaglyph.fx" "$OUT/"
sha256sum "$OUT/sourcevr.so"
