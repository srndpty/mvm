#!/usr/bin/env bash
# P3-5A: 数式 group の transition (src/media/mlt/mvm_mlt_math_group.c) を、MLT 7.36.1 と一緒に
# Linux の gcc の検出器 (ThreadSanitizer、AddressSanitizer + UBSan) の下で動かす。
#
# MLT は framework と core module だけを、同じ検出器付きで source から build する
# (color producer、null consumer、tractor、playlist を使う)。Qt・FFmpeg の module は build しない。
# mvm_mlt_export.c は Windows の API と Qt・FFmpeg の module に依存するので対象外。
#
# 使い方 (WSL の中で、repository の root から):
#   scripts/math-group-p35a-sanitizers.sh <作業用の directory (Linux の file system)> <出力先>
set -u
work="${1:?作業用の directory を指定します}"
out="${2:?出力先の directory を指定します}"
repo="$(pwd)"
mkdir -p "$work" "$out"
status=0
tarball_sha256=24f4136e3ca56d271bf8bc262644acacf396d5dfce026b610999e7c5cdfa3c2e

if [ ! -f "$work/mlt.tar.gz" ]; then
    curl -sL https://github.com/mltframework/mlt/archive/refs/tags/v7.36.1.tar.gz -o "$work/mlt.tar.gz" || exit 2
fi
echo "$tarball_sha256  $work/mlt.tar.gz" | sha256sum -c - > "$out/mlt-source-check.txt" 2>&1 || { echo "MLT の source の SHA-256 が違います"; exit 2; }
[ -d "$work/mlt-7.36.1" ] || tar xzf "$work/mlt.tar.gz" -C "$work"

# MLT の CMake は pkg-config を必須にしている。build する範囲 (framework と core) は外部の
# library を使わないので、無い環境では「何も見つからない」と答える代わりを置く (package を入れない)。
if ! command -v pkg-config > /dev/null; then
    mkdir -p "$work/bin"
    printf '#!/bin/sh
case "$1" in --version) echo 0.29.2; exit 0;; esac
exit 1
' > "$work/bin/pkg-config"
    chmod +x "$work/bin/pkg-config"
    PATH="$work/bin:$PATH"
    echo "pkg-config: 代わり (何も見つからないと答える)" > "$out/pkg-config.txt"
fi

sources="tests/sanitizer/math_group_mlt_lifecycle.cpp src/media/math/math_group_compose.cpp"
{
    echo "compiler: $(g++ --version | head -1)"
    echo "mlt: v7.36.1 (tarball sha256 $tarball_sha256)"
    sha256sum $sources src/media/mlt/mvm_mlt_math_group.c src/media/mlt/mvm_mlt_math_group.h \
        src/media/mlt/mvm_mlt_export.h src/media/math/math_group_compose.h tests/sanitizer/shim/windows.h \
        scripts/math-group-p35a-sanitizers.sh
} > "$out/identity.txt"

# 検出器ごとに、MLT (framework + core) と harness を build する。
build() { # 名前 検出器の flag
    local name="$1" flags="$2"
    local prefix="$work/install-$name"
    if [ ! -f "$prefix/lib/libmlt-7.so" ]; then
        cmake -S "$work/mlt-7.36.1" -B "$work/build-$name" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_C_FLAGS="$flags -fno-omit-frame-pointer" \
            -DCMAKE_CXX_FLAGS="$flags -fno-omit-frame-pointer" -DCMAKE_SHARED_LINKER_FLAGS="$flags" \
            -DCMAKE_MODULE_LINKER_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" \
            -DMOD_AVFORMAT=OFF -DMOD_DECKLINK=OFF -DMOD_FREI0R=OFF -DMOD_GDK=OFF -DMOD_JACKRACK=OFF \
            -DUSE_LV2=OFF -DUSE_VST2=OFF -DMOD_KDENLIVE=OFF -DMOD_NORMALIZE=OFF -DMOD_OLDFILM=OFF \
            -DMOD_MOVIT=OFF -DMOD_PLUS=OFF -DMOD_PLUSGPL=OFF -DMOD_QT6=OFF -DMOD_RESAMPLE=OFF \
            -DMOD_RTAUDIO=OFF -DMOD_RUBBERBAND=OFF -DMOD_SDL2=OFF -DMOD_SOX=OFF -DMOD_VIDSTAB=OFF \
            -DMOD_VORBIS=OFF -DMOD_XINE=OFF -DMOD_XML=OFF -DCLANG_FORMAT=OFF \
            > "$out/mlt-configure-$name.log" 2>&1 || { echo "MLT ($name) の configure に失敗"; return 1; }
        cmake --build "$work/build-$name" -j 8 --target install > "$out/mlt-build-$name.log" 2>&1 ||
            { echo "MLT ($name) の build に失敗"; return 1; }
    fi
    local common="-O1 -g -fno-omit-frame-pointer $flags -pthread -I tests/sanitizer/shim -I src -I $prefix/include/mlt-7"
    gcc -std=gnu11 $common -c src/media/mlt/mvm_mlt_math_group.c -o "$work/math_group-$name.o" \
        > "$out/build-$name.log" 2>&1 || { echo "transition ($name) の build に失敗"; return 1; }
    g++ -std=c++20 $common $sources "$work/math_group-$name.o" -L "$prefix/lib" -lmlt-7 \
        -Wl,-rpath,"$prefix/lib" -o "$work/lifecycle-$name" >> "$out/build-$name.log" 2>&1 ||
        { echo "harness ($name) の build に失敗"; return 1; }
}

run() { # 検出器の名前 scenario worker 期待 (ok = 成功して報告 0、report = 検出器が報告)
    local name="$1" scenario="$2" workers="$3" expect="$4"
    local log="$out/$name-$scenario-w$workers.log"
    TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1" \
        ASAN_OPTIONS="detect_leaks=1 abort_on_error=0" UBSAN_OPTIONS="print_stacktrace=1" \
        "$work/lifecycle-$name" "$work/install-$name/lib/mlt-7" "$scenario" "$workers" > "$log" 2>&1
    local code=$?
    local races uaf other
    races=$(grep -c "WARNING: ThreadSanitizer: data race" "$log")
    uaf=$(grep -c "ERROR: AddressSanitizer: heap-use-after-free" "$log")
    other=$(grep -c "WARNING: ThreadSanitizer\|ERROR: AddressSanitizer\|ERROR: LeakSanitizer\|runtime error:" "$log")
    local ok=0
    case "$expect" in
        ok) [ "$code" -eq 0 ] && [ "$other" -eq 0 ] && ok=1 ;;
        race) [ "$races" -gt 0 ] && ok=1 ;;
        uaf) [ "$uaf" -gt 0 ] && [ "$code" -ne 0 ] && ok=1 ;;
    esac
    echo "$name $scenario workers=$workers: exit=$code data race=$races 解放後の参照=$uaf 報告の合計=$other 期待=$expect 期待どおり=$ok"
    [ "$ok" -eq 1 ] || status=1
}

build tsan "-fsanitize=thread" || exit 2
build asan "-fsanitize=address,undefined" || exit 2

for name in tsan asan; do
    for workers in 1 4 8; do
        run "$name" normal "$workers" ok
        run "$name" cancel "$workers" ok
        run "$name" error "$workers" ok
    done
    run "$name" init-failure 1 ok
    run "$name" frame-outlives 1 ok
done
# 対照: 検出器が報告しなければ、検証の方法が働いていない。
run asan control-frame-ref 1 uaf
run asan control-raster-freed 1 uaf
run tsan control-shared-write 8 race
# 記録: MLT 7.36.1 の color producer の中の data race (数式 group の code の外)。cache を先に
# 作らないと、検出器が producer_colour.c の中の race を報告する。
run tsan mlt-color-race 8 race

echo "不成立: $status"
exit $status
