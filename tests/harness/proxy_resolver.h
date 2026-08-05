/*
 * mvm Phase 0 / S7 - proxy path resolver
 *
 * 位置づけ:
 *   Phase 0 の検証用。MLT にも Qt にも依存しない純粋なロジックであり、
 *   決定論的に単体テストできる。**製品用 Project JSON へ広げないこと。**
 *
 * 設計上の要点:
 *
 *   proxy 情報を MLT XML や MLT の property の正本にしない。
 *   どのファイルを開くかは **MLT graph を構築する前に** ここで決める。
 *   MLT へ渡す時点では、resource は既に proxy か original に確定している。
 *
 *   同じ logical timeline から
 *     - original scenario
 *     - proxy-resolved scenario
 *   の両方を生成できる。timeline 側は「どの素材か」だけを持ち、
 *   「どのファイルか」は持たない。
 *
 *   final は **常に original** を返す。proxy が有効でも変わらない。
 *   preview と final で解決先が違うことが、この resolver の存在理由である。
 *
 *   未知の id は解決しない (fail-closed)。
 *   「見つからなければ元のパスをそのまま使う」のような黙った fallback を
 *   入れると、proxy が効いていないことに気づけないまま性能を測ることになる。
 */

#ifndef MVM_PROXY_RESOLVER_H
#define MVM_PROXY_RESOLVER_H

#include <map>
#include <string>
#include <vector>

namespace bench {

enum class ResolveTarget {
    Preview, // 編集中のプレビュー。proxy が有効なら proxy を使う
    Final,   // 最終書き出し。**必ず original**
};

struct ProxyEntry {
    std::string sourceId;     // logical な素材 id。scenario 上の source 文字列
    std::string originalPath; // 元素材
    std::string proxyPath;    // proxy。空なら proxy 無し
    bool proxyEnabled = true; // proxy を使う設定か
};

enum class ResolveStatus {
    UsedProxy,       // preview で proxy を使った
    UsedOriginal,    // original を使った
    UnknownSourceId, // 未登録。呼び出し側が失敗させること
};

struct ResolveResult {
    ResolveStatus status = ResolveStatus::UnknownSourceId;
    std::string path;
    std::string reason;

    bool ok() const { return status != ResolveStatus::UnknownSourceId; }

    bool usedProxy() const { return status == ResolveStatus::UsedProxy; }
};

class ProxyResolver {
public:
    // 同じ id を二度登録したら false。上書きを黙って許すと
    // 「どちらが効いているのか」が分からなくなる。
    bool add(const ProxyEntry& e) {
        if (e.sourceId.empty())
            return false;
        return entries_.emplace(e.sourceId, e).second;
    }

    bool has(const std::string& sourceId) const { return entries_.count(sourceId) != 0; }

    size_t size() const { return entries_.size(); }

    ResolveResult resolve(const std::string& sourceId, ResolveTarget target) const {
        ResolveResult r;
        auto it = entries_.find(sourceId);
        if (it == entries_.end()) {
            r.status = ResolveStatus::UnknownSourceId;
            r.reason = "未登録の素材 id: " + sourceId;
            return r;
        }
        const ProxyEntry& e = it->second;

        // final は proxy 設定に関係なく original。
        if (target == ResolveTarget::Final) {
            r.status = ResolveStatus::UsedOriginal;
            r.path = e.originalPath;
            r.reason = "final は常に original";
            return r;
        }
        if (!e.proxyEnabled) {
            r.status = ResolveStatus::UsedOriginal;
            r.path = e.originalPath;
            r.reason = "proxy が無効";
            return r;
        }
        if (e.proxyPath.empty()) {
            r.status = ResolveStatus::UsedOriginal;
            r.path = e.originalPath;
            r.reason = "proxy が未生成";
            return r;
        }
        r.status = ResolveStatus::UsedProxy;
        r.path = e.proxyPath;
        r.reason = "preview は proxy";
        return r;
    }

    std::vector<std::string> ids() const {
        std::vector<std::string> out;
        out.reserve(entries_.size());
        for (const auto& kv : entries_)
            out.push_back(kv.first);
        return out;
    }

private:
    std::map<std::string, ProxyEntry> entries_;
};

} // namespace bench

#endif // MVM_PROXY_RESOLVER_H
