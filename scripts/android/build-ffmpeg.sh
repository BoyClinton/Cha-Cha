#!/usr/bin/env bash
# Builds FFmpeg's H.264 decoder (libavcodec + libavutil, static, arm64-v8a) for the Android build (docs/android.md).
# Runs in WSL (FFmpeg's configure needs a POSIX shell) with the Linux NDK; scripts/build-android.ps1 calls it.
# Usage: build-ffmpeg.sh <install prefix>
# Needs: ~/android-deps/android-ndk-r30 and ~/android-deps/ffmpeg-<version> (tools/README.md § Android), make.
set -euo pipefail
dest="$1"  # may contain spaces (the checkout): FFmpeg is installed on the Linux disk and copied there
deps="${ANDROID_DEPS:-$HOME/android-deps}"
prefix="$deps/ffmpeg-arm64-v8a"
ndk="${ANDROID_NDK_LINUX:-$deps/android-ndk-r30}"
api=29
src="$(ls -d "$deps"/ffmpeg-[0-9]*/ | sort -V | tail -1)"  # ffmpeg-<version>, not ffmpeg-build-* / ffmpeg-arm64-*
tc="$ndk/toolchains/llvm/prebuilt/linux-x86_64"
work="$deps/ffmpeg-build-arm64"
copy_out() {
  mkdir -p "$dest/lib" "$dest/include"
  cp "$prefix"/lib/libavcodec.a "$prefix"/lib/libavutil.a "$dest/lib/"
  cp -r "$prefix"/include/libavcodec "$prefix"/include/libavutil "$dest/include/"
  echo "FFmpeg: $dest"
}
if [ -f "$prefix/lib/libavcodec.a" ] && [ "$prefix/lib/libavcodec.a" -nt "$0" ]; then copy_out; exit 0; fi
rm -rf "$work" "$prefix" && mkdir -p "$work" && cd "$work"
# only what h264.cpp uses: the H.264 decoder (no formats, filters, scalers, programs; nothing autodetected, so no
# MediaCodec / JNI dependencies)
"$src/configure" --prefix="$prefix" \
  --target-os=android --arch=aarch64 --cpu=armv8-a --enable-cross-compile \
  --cc="$tc/bin/aarch64-linux-android$api-clang" --cxx="$tc/bin/aarch64-linux-android$api-clang++" \
  --ar="$tc/bin/llvm-ar" --ranlib="$tc/bin/llvm-ranlib" --nm="$tc/bin/llvm-nm" --strip="$tc/bin/llvm-strip" \
  --sysroot="$tc/sysroot" \
  --enable-static --disable-shared --enable-pic --disable-debug --disable-doc --disable-programs \
  --disable-autodetect --disable-everything --disable-network \
  --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample \
  --enable-decoder=h264 --enable-parser=h264 --enable-neon --enable-asm
make -j"$(nproc)"
make install
copy_out
