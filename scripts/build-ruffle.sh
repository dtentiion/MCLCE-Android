#!/usr/bin/env bash
# Builds third_party/ruffle_android for arm64 with the NDK's clang. The
# static library it leaves in target/ is what CMake links.
#
# Needs: rustup target add aarch64-linux-android

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [ -z "$NDK" ]; then
    SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
    NDK="$(ls -d "$SDK"/ndk/* 2>/dev/null | sort | tail -1)"
fi
if [ ! -d "$NDK" ]; then
    echo "build-ruffle: set ANDROID_NDK_HOME"
    exit 1
fi

case "$(uname -s)" in
    Linux*) HOST=linux-x86_64; EXT="" ;;
    Darwin*) HOST=darwin-x86_64; EXT="" ;;
    *) HOST=windows-x86_64; EXT=".cmd" ;;
esac
BIN="$NDK/toolchains/llvm/prebuilt/$HOST/bin"
API=26

export CC_aarch64_linux_android="$BIN/aarch64-linux-android$API-clang$EXT"
export CXX_aarch64_linux_android="$BIN/aarch64-linux-android$API-clang++$EXT"
export AR_aarch64_linux_android="$BIN/llvm-ar"
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$CC_aarch64_linux_android"

cd "$REPO_ROOT/third_party/ruffle_android"
cargo build --release --target aarch64-linux-android
ls -la target/aarch64-linux-android/release/libruffle_android.a
