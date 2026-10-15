#!/usr/bin/env bash
# P3-5A.2: V1 の背景の保護 (src/media/mlt/mvm_mlt_v1_background.c) を、MLT 7.36.1 と一緒に
# Linux の gcc の検出器 (ThreadSanitizer、AddressSanitizer + UBSan) の下で動かす。
#
# MLT の build は scripts/math-group-p35a-sanitizers.sh と同じもの (framework と core module を
# 検出器付きで source から build) を使う。無ければ、先にその script を実行する。
#
# 使い方 (WSL の中で、repository の root から):
#   scripts/mlt-color-race-p35a2-sanitizers.sh <作業用の directory> <出力先>
set -u
work="${1:?作業用の directory を指定します}"
out="${2:?出力先の directory を指定します}"
mkdir -p "$out"
status=0
for name in tsan asan; do
    [ -f "$work/install-$name/lib/libmlt-7.so" ] || { echo "MLT ($name) の build がありません。先に scripts/math-group-p35a-sanitizers.sh を実行します"; exit 2; }
done

sources="tests/sanitizer/v1_background_mlt_race.cpp"
{
    echo "compiler: $(g++ --version | head -1)"
    echo "mlt: v7.36.1 (tarball sha256 $(sha256sum "$work/mlt.tar.gz" | cut -d' ' -f1))"
    sha256sum $sources src/media/mlt/mvm_mlt_v1_background.c src/media/mlt/mvm_mlt_v1_background.h \
        tests/sanitizer/shim/windows.h scripts/mlt-color-race-p35a2-sanitizers.sh \
        "$work/mlt-7.36.1/src/modules/core/producer_colour.c" \
        "$work/install-tsan/lib/libmlt-7.so" "$work/install-tsan/lib/mlt-7/libmltcore.so" \
        "$work/install-asan/lib/libmlt-7.so" "$work/install-asan/lib/mlt-7/libmltcore.so"
} > "$out/identity.txt"

build() { # 名前 検出器の flag
    local name="$1" flags="$2"
    local prefix="$work/install-$name"
    local common="-O1 -g -fno-omit-frame-pointer $flags -pthread -I tests/sanitizer/shim -I src -I $prefix/include/mlt-7"
    gcc -std=gnu11 $common -c src/media/mlt/mvm_mlt_v1_background.c -o "$work/v1_background-$name.o" \
        > "$out/build-$name.log" 2>&1 || { echo "背景の module ($name) の build に失敗"; return 1; }
    g++ -std=c++20 $common $sources "$work/v1_background-$name.o" -L "$prefix/lib" -lmlt-7 \
        -Wl,-rpath,"$prefix/lib" -o "$work/v1-background-$name" >> "$out/build-$name.log" 2>&1 ||
        { echo "harness ($name) の build に失敗"; return 1; }
}

# 期待:
#   ok    成功して、検出器の報告が 0
#   gate  harness 自身の判定だけを見る (P3-5A.2 の最初の版で、cache の作り直しを実際に起こす
#         対照に使った。今は、key を変える要求を MLT へ渡す前に断るので、使っていない)
#   race  検出器が producer_colour.c の中の data race を報告する
run() { # 検出器の名前 scenario worker 期待 [番号]
    local name="$1" scenario="$2" workers="$3" expect="$4" index="${5:-}"
    local log="$out/$name-$scenario-w$workers${index:+-$index}.log"
    TSAN_OPTIONS="halt_on_error=0" ASAN_OPTIONS="detect_leaks=1 abort_on_error=0" \
        UBSAN_OPTIONS="print_stacktrace=1" \
        "$work/v1-background-$name" "$work/install-$name/lib/mlt-7" "$scenario" "$workers" > "$log" 2>&1
    local code=$?
    local races colour other
    races=$(grep -c "WARNING: ThreadSanitizer: data race" "$log")
    colour=$(grep -c "producer_colour.c" "$log")
    other=$(grep -c "WARNING: ThreadSanitizer\|ERROR: AddressSanitizer\|ERROR: LeakSanitizer\|runtime error:" "$log")
    local ok=0
    case "$expect" in
        ok) [ "$code" -eq 0 ] && [ "$other" -eq 0 ] && ok=1 ;;
        # 検出器が報告すると終了コードが変わる (ThreadSanitizer は 66) ので、harness 自身の判定を読む。
        gate) grep -q "^$scenario: 成立" "$log" && ok=1 ;;
        race) [ "$races" -gt 0 ] && [ "$colour" -gt 0 ] && ok=1 ;;
    esac
    echo "$name $scenario workers=$workers${index:+ #$index}: exit=$code data race=$races (producer_colour.c の行=$colour) 報告の合計=$other 期待=$expect 期待どおり=$ok"
    [ "$ok" -eq 1 ] || status=1
}

build tsan "-fsanitize=thread" || exit 2
build asan "-fsanitize=address,undefined" || exit 2

for name in tsan asan; do
    for workers in 1 4 8; do
        run "$name" protected "$workers" ok
        run "$name" protected-rgba "$workers" ok
        run "$name" seek "$workers" ok
        run "$name" cancel "$workers" ok
    done
    run "$name" fault-prewarm 1 ok
    run "$name" fault-rebuild 8 ok
    # cache の key を変える要求。MLT へ渡す前に断るので、cache は作り直されず、検出器の報告も無い。
    run "$name" invalidate-format 8 ok
    run "$name" invalidate-size 8 ok
    run "$name" invalidate-rgba64 8 ok
    run "$name" invalidate-resource 8 ok
done

# 対照: 保護なし。検出器が MLT の color producer の中の data race を報告しなければ、検証の方法が
# 働いていない。
run tsan unprotected 4 race
run tsan unprotected 8 race

# 対照: 保護なしで、誤った画素 (黒でない背景) を実際に受け取るか。timing によるので、合否には
# 使わず、何回再現したかを記録する (終了コード 3 = 再現)。
reproduced=0
tries=10
for index in $(seq 1 $tries); do
    "$work/v1-background-asan" "$work/install-asan/lib/mlt-7" unprotected-pixels 8 \
        > "$out/asan-unprotected-pixels-w8-$index.log" 2>&1
    [ $? -eq 3 ] && reproduced=$((reproduced + 1))
done
echo "asan unprotected-pixels workers=8: 黒でない画素を受け取った run = $reproduced / $tries (記録のみ)"
echo "$reproduced / $tries" > "$out/unprotected-pixels-summary.txt"

echo "不成立: $status"
exit $status
