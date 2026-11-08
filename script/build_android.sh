#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-debug}"
SDK_ROOT="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/android-build/android-sdk}}"
NDK_ROOT="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-$SDK_ROOT/ndk/26.3.11579264}}"
JAVA_ROOT="${JAVA_HOME:-$HOME/.local/lib/jdk-21.0.13+9/Contents/Home}"
VERSION="$(sed -n "s/^[[:space:]]*versionName \"\\([^\"]*\\)\"/\\1/p" \
    "$ROOT_DIR/android/app/build.gradle" | head -1)"

export ANDROID_HOME="$SDK_ROOT"
export ANDROID_SDK_ROOT="$SDK_ROOT"
export ANDROID_NDK_HOME="$NDK_ROOT"
export ANDROID_NDK_ROOT="$NDK_ROOT"
export JAVA_HOME="$JAVA_ROOT"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"

[[ -x "$ANDROID_HOME/platform-tools/adb" ]] \
    || { echo "Android SDK not found: $ANDROID_HOME" >&2; exit 2; }
[[ -x "$NDK_ROOT/ndk-build" ]] \
    || { echo "Android NDK not found: $NDK_ROOT" >&2; exit 2; }

cd "$ROOT_DIR/android"
case "$MODE" in
    debug)
        ./gradlew :app:assembleDebug
        source_apk="$ROOT_DIR/android/app/build/outputs/apk/debug/app-debug.apk"
        output_apk="$ROOT_DIR/dist/CadLens-${VERSION}-android-arm64-debug.apk"
        ;;
    release)
        ./gradlew :app:assembleRelease
        source_apk="$ROOT_DIR/android/app/build/outputs/apk/release/app-release.apk"
        output_apk="$ROOT_DIR/dist/CadLens-${VERSION}-android-arm64.apk"
        ;;
    *)
        echo "usage: $0 [debug|release]" >&2
        exit 2
        ;;
esac

mkdir -p "$ROOT_DIR/dist"
cp "$source_apk" "$output_apk"
echo "$output_apk"
