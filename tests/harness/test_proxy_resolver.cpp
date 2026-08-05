// mvm Phase 0 / S7 - ProxyResolver の単体テスト
//
// この resolver の存在理由は「preview と final で解決先が違う」ことなので、
// そこを最も厚く検査する。final が proxy を返したら preview/final 一致
// (M11) が構造的に壊れる。

#include "proxy_resolver.h"

#include <cstdio>
#include <string>

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool cond, const std::string& what) {
    gChecks++;
    if (!cond) {
        gFailures++;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

using namespace bench;

ProxyResolver makeResolver() {
    ProxyResolver r;
    r.add({"v4k60_h264.mp4", "C:/media/v4k60_h264.mp4", "C:/media/_proxy/v4k60_gop12.mp4", true});
    r.add({"v1080p60_h264.mp4", "C:/media/v1080p60_h264.mp4", "", true}); // proxy 未生成
    r.add({"wav_48k.wav", "C:/media/wav_48k.wav", "C:/media/_proxy/wav.wav", false}); // 無効
    return r;
}

void testFinalAlwaysOriginal() {
    std::fprintf(stderr, "[final は常に original]\n");
    ProxyResolver r = makeResolver();

    // proxy があって有効でも、final は original
    auto f = r.resolve("v4k60_h264.mp4", ResolveTarget::Final);
    check(f.ok(), "解決できる");
    check(!f.usedProxy(), "final は proxy を使わない");
    check(f.path == "C:/media/v4k60_h264.mp4", "final は original パス");

    // 同じ id の preview は proxy
    auto p = r.resolve("v4k60_h264.mp4", ResolveTarget::Preview);
    check(p.usedProxy(), "preview は proxy を使う");
    check(p.path == "C:/media/_proxy/v4k60_gop12.mp4", "preview は proxy パス");

    // 同じ id で preview と final が違うこと自体を検査する
    check(p.path != f.path, "preview と final の解決先が違う");
}

void testProxyNotGenerated() {
    std::fprintf(stderr, "[proxy 未生成なら original]\n");
    ProxyResolver r = makeResolver();
    auto p = r.resolve("v1080p60_h264.mp4", ResolveTarget::Preview);
    check(p.ok(), "解決できる");
    check(!p.usedProxy(), "proxy パスが空なら proxy を使わない");
    check(p.path == "C:/media/v1080p60_h264.mp4", "original へ落ちる");
}

void testProxyDisabled() {
    std::fprintf(stderr, "[proxy 無効なら original]\n");
    ProxyResolver r = makeResolver();
    auto p = r.resolve("wav_48k.wav", ResolveTarget::Preview);
    check(p.ok(), "解決できる");
    check(!p.usedProxy(), "無効なら proxy を使わない");
    check(p.path == "C:/media/wav_48k.wav", "original へ落ちる");
}

void testUnknownIsFailClosed() {
    std::fprintf(stderr, "[未知の id は fail-closed]\n");
    ProxyResolver r = makeResolver();
    auto p = r.resolve("no_such.mp4", ResolveTarget::Preview);
    check(!p.ok(), "未知の id は解決しない");
    check(p.status == ResolveStatus::UnknownSourceId, "UnknownSourceId が返る");
    check(p.path.empty(), "パスを勝手に作らない");

    // final でも同じ。「final なら元パスをそのまま」という抜け道を作らない。
    auto f = r.resolve("no_such.mp4", ResolveTarget::Final);
    check(!f.ok(), "final でも未知の id は解決しない");
}

void testDuplicateRejected() {
    std::fprintf(stderr, "[同じ id の二重登録を拒否]\n");
    ProxyResolver r;
    check(r.add({"a.mp4", "C:/a.mp4", "C:/p/a.mp4", true}), "1 回目は登録できる");
    check(!r.add({"a.mp4", "C:/other.mp4", "", true}), "2 回目は拒否される");
    check(r.size() == 1, "登録数は 1 のまま");
    auto p = r.resolve("a.mp4", ResolveTarget::Preview);
    check(p.path == "C:/p/a.mp4", "最初の登録が生きている");

    check(!r.add({"", "C:/x.mp4", "", true}), "空 id は登録できない");
}

void testAllIdsResolvable() {
    std::fprintf(stderr, "[登録した id はすべて両 target で解決できる]\n");
    ProxyResolver r = makeResolver();
    for (const auto& id : r.ids()) {
        check(r.resolve(id, ResolveTarget::Preview).ok(), "preview: " + id);
        auto f = r.resolve(id, ResolveTarget::Final);
        check(f.ok(), "final: " + id);
        check(!f.usedProxy(), "final で proxy を使っていない: " + id);
    }
}

} // namespace

int main() {
    testFinalAlwaysOriginal();
    testProxyNotGenerated();
    testProxyDisabled();
    testUnknownIsFailClosed();
    testDuplicateRejected();
    testAllIdsResolvable();

    std::fprintf(stderr, "\n%d 検査中 %d 件失敗\n", gChecks, gFailures);
    if (gFailures == 0)
        std::fprintf(stderr, "ProxyResolver 単体テスト: 全て通過\n");
    return gFailures == 0 ? 0 : 1;
}
