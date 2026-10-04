// mvm_sha256 の結果を FIPS 180-2 の既知の値と照合する。期待値は実装から作らない。

#include "util/mvm_sha256.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

std::string oneShot(const std::string& text) {
    char out[MVM_SHA256_HEX_SIZE] = {};
    if (mvm_sha256_hex(text.data(), text.size(), out) != 0)
        return "<error>";
    return out;
}

} // namespace

int main() {
    check(oneShot("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "空文字列");
    check(oneShot("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "abc");
    check(oneShot("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "448 bit の message");

    // 100 万個の 'a' を 1000 byte ずつ渡す (分割した update が 1 回の入力と同じになること)。
    long status = 0;
    MvmSha256* hash = mvm_sha256_create(&status);
    check(hash != nullptr && status == 0, "hash を作れる");
    if (hash) {
        char chunk[1000];
        std::memset(chunk, 'a', sizeof(chunk));
        bool updated = true;
        for (int i = 0; i < 1000; ++i)
            updated = updated && mvm_sha256_update(hash, chunk, sizeof(chunk)) == 0;
        check(updated, "分割した update が成功する");
        char out[MVM_SHA256_HEX_SIZE] = {};
        check(mvm_sha256_finish_hex(hash, out) == 0 &&
                  std::string(out) ==
                      "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
              "100 万個の a");
        check(mvm_sha256_update(hash, "x", 1) != 0, "確定後の update は失敗する");
        check(mvm_sha256_finish_hex(hash, out) != 0, "2 回目の確定は失敗する");
        mvm_sha256_destroy(hash);
    }
    check(mvm_sha256_update(nullptr, "x", 1) != 0, "NULL の hash への update は失敗する");

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
