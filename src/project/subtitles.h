#ifndef MVM_PROJECT_SUBTITLES_H
#define MVM_PROJECT_SUBTITLES_H

#include "project/project.h"

#include <functional>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace mvm::project {

struct SubtitleTimedText {
    std::int64_t startMs = 0, endMs = 0;
    std::string content;
};

bool subtitleCuesFromTimedText(const std::vector<SubtitleTimedText>& segments, std::int64_t fpsNum,
                               std::int64_t fpsDen, std::int64_t speedNum, std::int64_t speedDen,
                               std::int64_t offsetFrame, const std::function<std::string()>& newId,
                               std::vector<SubtitleCue>& cues, std::string& error);
bool validateSubtitles(const Project& project, std::string& error);
const SubtitleCue* activeSubtitleAt(const Project& project, std::int64_t frame);
bool subtitleContainsFrame(const SubtitleCue& cue, std::int64_t frame);
// 最近傍へ丸め、同率は後方へ送る。範囲外は失敗する。
bool subtitleTimeToFrame(std::int64_t milliseconds, std::int64_t fpsNum, std::int64_t fpsDen,
                         std::int64_t& frame, std::string& error);
bool subtitleFrameToTime(std::int64_t frame, std::int64_t fpsNum, std::int64_t fpsDen,
                         std::int64_t& milliseconds, std::string& error);
bool parseSrt(std::string_view text, std::int64_t fpsNum, std::int64_t fpsDen,
              const std::function<std::string()>& newId, std::vector<SubtitleCue>& cues,
              std::string& error);
bool writeSrt(const Project& project, std::string& text, std::string& error);
// 認識結果とSRTの適用は同じ検証・並べ替えを通す。
bool applySubtitleCues(Project& project, std::vector<SubtitleCue> cues, bool replace,
                       std::string& error);
bool editSubtitle(Project& project, SubtitleCue cue, std::string& error);
bool deleteSubtitle(Project& project, const std::string& id, std::string& error);
// frame で区間を分け、本文も前後に分ける。textBreak は本文を分ける文字位置 (Unicode の
// 文字数、両端を除く)。省略すると、区間の中の frame の比率に最も近い句読点・空白・改行の
// 直後で分け、それも無ければ比率の位置で分ける。1 文字の本文は両方に残す。
bool splitSubtitle(Project& project, const std::string& id, std::int64_t frame,
                   const std::string& newId, std::string& error,
                   std::optional<std::size_t> textBreak = std::nullopt);
// 本文の分け方 (splitSubtitle が使う)。試験と UI の説明のため公開する。
std::pair<std::string, std::string> splitSubtitleText(const std::string& content, double ratio,
                                                      std::optional<std::size_t> textBreak);
bool mergeSubtitle(Project& project, const std::string& id, std::string& error);
bool editSubtitleTime(Project& project, std::int64_t start, std::int64_t removed,
                      std::int64_t inserted, const std::function<std::string()>& newId,
                      std::string& error);

// clip とのリンク。timeline 編集の候補に対して呼ぶ (検証は呼び出し側の finalize で行う)。
// clipIds のいずれかにリンクした字幕の最小開始 frame。無ければ nullopt。
std::optional<std::int64_t> linkedSubtitleStart(const Project& project,
                                                const std::unordered_set<std::string>& clipIds);
// clipIds にリンクした字幕を delta だけ横へ動かす。他の字幕と重なる場合は失敗する。
bool shiftLinkedSubtitles(Project& project, const std::unordered_set<std::string>& clipIds,
                          std::int64_t delta, std::string& error);
// clip を削除・カットしたときに、その clip にリンクした字幕を消す。
void eraseLinkedSubtitles(Project& project, const std::unordered_set<std::string>& clipIds);
// clip を frame で分割したときのリンクの付け替え。分割位置以降に始まる字幕は右半分の clip へ
// リンクを移す。分割位置を跨ぐ字幕は frame で 2 つに分け (本文も splitSubtitleText で分ける)、
// 前半を左、後半を右の clip へリンクする。後から片方の clip を消しても、その clip の上に
// あった部分だけが消える。
void relinkSubtitlesAfterSplit(Project& project, const std::string& leftId,
                               const std::string& rightId, std::int64_t frame,
                               const std::function<std::string()>& newId);
// 時間の対応を変える clip の編集 (trim・ロール・スリップ・スライド・レート調整・速度変更・
// リップルトリム) の後で、リンクした字幕を音声に合わせて置き直す。字幕は素材の時刻を保つ:
// 編集前の clip で字幕の区間を素材の時刻へ直し、編集で素材の範囲から外れた部分 (trim で
// 切り落とした部分) を除き、編集後の clip で timeline の区間へ戻す。素材の範囲から全部
// 外れた字幕は消す。編集前の位置から作り直すので、リップルで既にずらした字幕も二重には
// ずらさない。他の字幕と重なる場合は失敗する (編集全体を拒否する)。
// 素材の範囲が残っているのに frame へ丸めると 0 frame になる字幕 (速度を上げすぎた場合) は
// 黙って消さず、編集全体を拒否する。
bool remapLinkedSubtitles(const Project& before, Project& candidate, std::string& error);
// clipIds にリンクした字幕のリンクを外す。外した件数を返す。
std::size_t unlinkSubtitles(Project& project, const std::unordered_set<std::string>& clipIds);
// 素材の削除・上書きなどで消えた clip へのリンクを外す (字幕自体は残す)。
void reconcileSubtitleLinks(Project& project);
} // namespace mvm::project
#endif
