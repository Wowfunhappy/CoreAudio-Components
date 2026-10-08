#!/bin/bash
# Builds static libavcodec/libavutil/libswresample (FFmpeg 4.4.6) with only the
# DTS (DCA) parser+decoder enabled, then installs the headers and .a libraries
# into DTS/DTSAudioComponent/ffmpeg/ so the component can link against them.
#
# Mirrors the recipe in "DTS/ffmpeg build notes.txt".
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"           # .../Audio-Components/DTS
DEST="$REPO_DIR/DTSAudioComponent/ffmpeg"           # where headers+libs go
WORK="$HOME/ffmpeg-dts-build"                        # scratch build area (outside repo)
NASM="${NASM:-$HOME/Desktop/nasm}"                   # assembler for FFmpeg's x86 SIMD
VER="4.4.6"
TARBALL="ffmpeg-$VER.tar.xz"
URL="https://ffmpeg.org/releases/$TARBALL"

mkdir -p "$WORK"
cd "$WORK"

if [ ! -f "$TARBALL" ]; then
    echo "==> Downloading $URL"
    curl -L -o "$TARBALL" "$URL"
fi

rm -rf "ffmpeg-$VER"
echo "==> Extracting"
tar xf "$TARBALL"
cd "ffmpeg-$VER"

echo "==> Configuring"
./configure \
    --prefix="$WORK/install" \
    --enable-static --disable-shared \
    --disable-all --enable-avcodec --enable-avutil --enable-swresample \
    --disable-programs --disable-doc \
    --enable-parser=dca --enable-decoder=dca \
    --disable-videotoolbox --disable-audiotoolbox --disable-iconv \
    --arch=x86_64 --x86asmexe="$NASM" \
    --extra-cflags="-arch x86_64 -mmacosx-version-min=10.6 -fPIC" \
    --extra-ldflags="-arch x86_64 -mmacosx-version-min=10.6"

echo "==> Building"
make -j"$(sysctl -n hw.ncpu)"
echo "==> Installing to staging prefix"
rm -rf "$WORK/install"
make install

echo "==> Copying headers + libs into $DEST"
rm -rf "$DEST"
mkdir -p "$DEST/include" "$DEST/lib"
cp -R "$WORK/install/include/." "$DEST/include/"
cp "$WORK/install/lib/libavcodec.a" "$DEST/lib/"
cp "$WORK/install/lib/libavutil.a" "$DEST/lib/"
cp "$WORK/install/lib/libswresample.a" "$DEST/lib/"

echo "==> Verifying dca decoder is present"
# Use grep -c (consumes all input) rather than grep -q, which exits early and would
# SIGPIPE nm — a false failure under 'set -o pipefail'.
DCA_COUNT=$(nm "$DEST/lib/libavcodec.a" | grep -c "ff_dca_decoder" || true)
if [ "$DCA_COUNT" -gt 0 ]; then
    echo "OK: ff_dca_decoder found in libavcodec.a"
else
    echo "ERROR: ff_dca_decoder NOT found in libavcodec.a" >&2
    exit 1
fi
echo "==> ffmpeg build complete"
