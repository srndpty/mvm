#!/usr/bin/env bash
# P3-3D: 数式の合成器 (src/media/math/math_group_compose) を、Linux の g++ の検出器の下で実行する。
# 対象は、MLT にも Windows にも依存しない部分 (合成器、準備済みの画像、画像の置き場) だけである。
# MLT の transition (mvm_mlt_math_group.c) と書き出しは、Windows の API と MLT に依存するので対象外。
#
# 使い方 (WSL の中で、repository の root から): scripts/math-transition-p33d-sanitizers.sh <出力先>
set -u
out="${1:?出力先の directory を指定します}"
mkdir -p "$out"
sources="tests/core/test_math_group_compose.cpp src/media/math/math_group_compose.cpp"
flags="-std=c++20 -O1 -g -pthread -I src"
status=0

{
    echo "compiler: $(g++ --version | head -1)"
    sha256sum $sources src/media/math/math_group_compose.h tests/harness/mlt_rgba_oracle.h
} > "$out/identity.txt"

run() { # 名前 期待 (0 = 成功、race = 検出器の報告) command...
    local name="$1" expect="$2"
    shift 2
    "$@" > "$out/$name.log" 2>&1
    local code=$?
    local races
    races=$(grep -c "WARNING: ThreadSanitizer" "$out/$name.log")
    local errors
    errors=$(grep -c "ERROR: AddressSanitizer\|runtime error:" "$out/$name.log")
    local ok=0
    if [ "$expect" = "race" ]; then
        [ "$races" -gt 0 ] && ok=1
    else
        [ "$code" -eq 0 ] && [ "$races" -eq 0 ] && [ "$errors" -eq 0 ] && ok=1
    fi
    echo "$name: exit=$code ThreadSanitizer の報告=$races AddressSanitizer・UBSan の報告=$errors 期待どおり=$ok"
    [ "$ok" -eq 1 ] || status=1
}

# ThreadSanitizer: data race。
g++ $flags -fsanitize=thread -o "$out/tsan" $sources > "$out/build-tsan.log" 2>&1 || { echo "tsan の build に失敗"; exit 2; }
# 同時の合成 (8 thread x 200 回。変えられない画像を共有して読む)。
run tsan-concurrent 0 env TSAN_OPTIONS=halt_on_error=0 "$out/tsan" concurrent
# 単体試験の全部 (画像の置き場、範囲だけを持つ画像、同時の合成を含む)。
run tsan-unit 0 env TSAN_OPTIONS=halt_on_error=0 "$out/tsan"
# 対照: 同じ合成先へ 2 thread が書く。検出器が報告しなければ、検出器が働いていない。
run tsan-race-control race env TSAN_OPTIONS=halt_on_error=0 "$out/tsan" race-control

# AddressSanitizer + UndefinedBehaviorSanitizer: 範囲外の読み書き、解放後の参照、未定義動作。
g++ $flags -fsanitize=address,undefined -fno-sanitize-recover=undefined -o "$out/asan" $sources > "$out/build-asan.log" 2>&1 || { echo "asan の build に失敗"; exit 2; }
run asan-unit 0 env ASAN_OPTIONS=detect_leaks=1 "$out/asan"
run asan-concurrent 0 env ASAN_OPTIONS=detect_leaks=1 "$out/asan" concurrent

echo "不成立: $status"
exit $status
