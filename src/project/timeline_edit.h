#ifndef MVM_PROJECT_TIMELINE_EDIT_H
#define MVM_PROJECT_TIMELINE_EDIT_H

#include "core/source_frame_mapping.h"
#include "project/project.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mvm::project {

struct TimelineEditResult {
    bool success = false;
    int selectedIndex = -1;
    std::string error;
};

struct TimelineFrameResult {
    bool success = false;
    std::int64_t frame = 0;
    std::string error;
};

struct TimelineValidationResult {
    bool success = false;
    std::int64_t totalFrames = 0;
    std::string error;
    // 同じ track の重なりを調べるために区間どうしを比べた回数。clip 数に対して線形に
    // 留まる (全組を比べない) ことの回帰検査に使う。
    std::uint64_t overlapComparisons = 0;
};

struct ClipKeyEditPreview {
    bool success = false;
    ClipEffects effects;
    std::int64_t frame = 0;
    std::string error;
};

// originalFrame が負なら追加。ドラッグ表示と確定の両方がこの候補計算を使う。
ClipKeyEditPreview previewClipKeyEdit(const Project& project, const std::string& clipId,
                                      ClipKeyKind kind, std::int64_t originalFrame,
                                      std::int64_t requestedFrame, double requestedPercent);
TimelineEditResult editClipKey(Project& project, const std::string& clipId, ClipKeyKind kind,
                               std::int64_t originalFrame, std::int64_t requestedFrame,
                               double requestedPercent);
// frame のキーを消す。最後のキーを消すと線は effects の基準値へ戻る。
TimelineEditResult deleteClipKey(Project& project, const std::string& clipId, ClipKeyKind kind,
                                 std::int64_t frame);

enum class TrimEdge { Left, Right };

// リンクされた clip (video / audio の 1 組) の相手にも同じ編集を適用するか。
// Premiere の「リンクされた選択」に相当する。既定の操作は Linked で、
// Alt を押しながらの操作は Single (操作した clip だけ) になる。
enum class LinkMode { Linked, Single };

TimelineFrameResult sourceBoundaryToTimelineBoundary(std::int64_t sourceFrame,
                                                     std::int64_t sourceFpsNum,
                                                     std::int64_t sourceFpsDen,
                                                     std::int64_t timelineFpsNum,
                                                     std::int64_t timelineFpsDen);
TimelineFrameResult timelineBoundaryToSourceBoundary(std::int64_t timelineFrame,
                                                     std::int64_t sourceFpsNum,
                                                     std::int64_t sourceFpsDen,
                                                     std::int64_t timelineFpsNum,
                                                     std::int64_t timelineFpsDen);
// clip の実効 fps (素材 fps × 速度、約分済み)。速度 s の clip は素材 fps が f s の clip と
// 同じに扱えるので、timeline との換算はすべてこれを使う。実際の素材 fps を使ってよいのは
// decode の検証と素材 frame の秒換算だけである。積を int64 で表せなければ nullopt。
std::optional<core::FrameRate> clipTimebase(const TimelineClip& clip);
// clipTimebase を使う境界換算。素材境界 -> timeline は ceil、逆は floor。
TimelineFrameResult clipSourceBoundaryToTimeline(const TimelineClip& clip, std::int64_t sourceFrame,
                                                 std::int64_t timelineFpsNum,
                                                 std::int64_t timelineFpsDen);
TimelineFrameResult clipTimelineBoundaryToSource(const TimelineClip& clip,
                                                 std::int64_t timelineFrame,
                                                 std::int64_t timelineFpsNum,
                                                 std::int64_t timelineFpsDen);
TimelineFrameResult timelineClipDuration(const Project& project, const TimelineClip& clip);
// clip 先頭から clipLocalFrame 番目の timeline frame が表示する素材 frame (素材の絶対 frame)。
// core::sourceFrameAtOutputPosition の四捨五入で、書き出し (MLT) と同じ frame を返す。
// 丸めで sourceOutFrame (trim で外した次の frame) を返すことがあるのも MLT と同じである。
// 素材の末尾を越える分だけは最終 frame に止める。
TimelineFrameResult clipSourceFrameAt(const TimelineClip& clip, std::int64_t timelineFpsNum,
                                      std::int64_t timelineFpsDen, std::int64_t clipLocalFrame);
TimelineFrameResult frameHoldProducerPosition(const TimelineClip& clip, std::int64_t timelineFpsNum,
                                              std::int64_t timelineFpsDen);
TimelineClip clipVideoSource(const TimelineClip& clip);

// 書き出しの producer に渡す cut の範囲。位置は素材の 0 frame から数えた output 位置
// (core/source_frame_mapping.h) で、[begin, end) が clipSourceFrameAt と同じ frame を出す。
// 素材の末尾で、丸めると存在しない frame を指す位置は cut に含めない。その分の
// tailFrames は最終 frame を繰り返して埋める (clipSourceFrameAt の最終 frame への止め方と同じ)。
struct ClipProducerRange {
    bool success = false;
    std::int64_t begin = 0;
    std::int64_t end = 0;
    std::int64_t tailFrames = 0;
    std::string error;
};

ClipProducerRange clipProducerRange(const TimelineClip& clip, std::int64_t timelineFpsNum,
                                    std::int64_t timelineFpsDen);
// フェードと音量カーブの評価に渡す素材 local frame (clipSourceFrameAt - in)。
// 丸めで素材範囲の外を指す分は [0, out - in) に収める。
TimelineFrameResult clipFadeSourceFrameAt(const TimelineClip& clip, std::int64_t timelineFpsNum,
                                          std::int64_t timelineFpsDen, std::int64_t clipLocalFrame);
bool sourceRateMatchesTimelineRate(const Project& project, const TimelineClip& clip);
TimelineValidationResult validateTimeline(const Project& project);

// clip ID -> timelineClips の位置。トランジションごとに全 clip を走査しない (clip 数 x
// トランジション数にしない) ために一度だけ作る。ID の文字列を参照で持つので、作った後に
// 元の Project の clip 列を変えてはいけない。ID が重複していれば先頭の位置を返す。
class ClipIdIndex {
public:
    explicit ClipIdIndex(const Project& project);
    // 無ければ -1。
    int find(std::string_view id) const;

private:
    std::unordered_map<std::string_view, int> indices_;
};

// 編集後の candidate のトランジションを clip に合わせる。clip が無い・同じ track で接して
// いない・フレーム保持を含むトランジションは消し、余白や尺が足りなければ縮める (0 frame に
// なれば消す)。track ごとに cut の昇順で処理するので結果は決まる。JSON の読み込みでは
// 呼ばない (読み込みは validateTimeline で fail-closed にする)。
void reconcileTimelineTransitions(Project& candidate);
// 編集の確定前に使う検証。reconcileTimelineTransitions の後に validateTimeline を行う。
TimelineValidationResult finalizeTimelineCandidate(Project& candidate);

int timelineClipIndexAt(const Project& project, TrackRef track, std::int64_t timelineFrame);
const TimelineClip* activeClipAt(const Project& project, TrackRef track,
                                 std::int64_t timelineFrame);
// index が track index と一致する配列を返す。clip が無い track は nullptr。
// 合成順は index 昇順で bottom -> top。
std::vector<const TimelineClip*> activeClipsAt(const Project& project, TrackKind kind,
                                               std::int64_t timelineFrame);
TimelineFrameResult timelineEndFrame(const Project& project);
TimelineFrameResult timelineTrackEndFrame(const Project& project, TrackRef track);

TimelineEditResult moveClip(Project& project, const std::string& clipId, TrackRef destinationTrack,
                            std::int64_t newStartFrame);
// anchor clip の移動量を選択 clip 全体へ適用する。Linked ならリンク相手も同じ時間差で
// 移動する。全変更を一つの candidate として検証する。
// newId を渡すと上書きで置く (Premiere の上書き)。動かした clip の下になる他の clip は、
// 丸ごと覆われれば消し (リンク相手は未リンクにする)、端が掛かれば削り、中に置けば 2 つに
// 分ける (右側は newId() の ID で未リンク)。newId が空なら重なりを拒否する。
TimelineEditResult moveClips(Project& project, const std::vector<std::string>& clipIds,
                             const std::string& anchorClipId, TrackRef destinationTrack,
                             std::int64_t newStartFrame, LinkMode linkMode,
                             const std::function<std::string()>& newId = {});
// track 末尾へ追加する。clip.track と clip.timelineStartFrame はここで確定させる。
TimelineEditResult appendTimelineClip(Project& project, TimelineClip clip, TrackRef track);
// 指定位置へ配置する。既存 clip と重なる場合は fail-closed にする。
TimelineEditResult placeTimelineClipAt(Project& project, TimelineClip clip, TrackRef track,
                                       std::int64_t timelineStartFrame);
// 再生ヘッドで最上位の active clip より上の非 mute 映像 track に文字・静止画を置く。
// 空きが無ければ映像 track を上へ足して置く。
TimelineEditResult placeStillClipAt(Project& project, TimelineClip clip,
                                    std::int64_t timelineStartFrame);
// 指定位置で空いている非 mute の audio track (A1 から順) に音声 clip を置く。
// 空きが無ければ audio track を足して置く。audio track が 1 本も無くても置ける。
TimelineEditResult placeAudioClipAt(Project& project, TimelineClip clip,
                                    std::int64_t timelineStartFrame);
// プロジェクトパネルや外部からのドロップで、指定した track・位置へ素材を置く。
//   target.index が track 数と等しければ、その種別の track を末尾に足して置く
//   linkedAudio があれば、ドロップした行の種別に合う側を target へ置き、相手は
//   もう一方の種別で空いている最初の非 mute track (無ければ新しい track) へ置く
//   target に置けない (種別が違う・既存の clip と重なる) ときは上書きせず、同じ種別で
//   空いている最初の非 mute track (無ければ新しい track) へ置く。時刻は変えない
//   開始位置が負なら 0 へ寄せる。target.index が範囲外なら失敗する
TimelineEditResult placeMediaAtDrop(Project& project, TimelineClip primary,
                                    std::optional<TimelineClip> linkedAudio, TrackRef target,
                                    std::int64_t timelineStartFrame);
// link済みvideo/audioを一つのcandidateへ追加し、Project invariantを満たした状態だけをcommitする。
TimelineEditResult placeLinkedAvPairAt(Project& project, TimelineClip video, TrackRef videoTrack,
                                       TimelineClip audio, TrackRef audioTrack,
                                       std::int64_t timelineStartFrame);
TimelineEditResult deleteTimelineClip(Project& project, int selectedIndex);
TimelineEditResult unlinkTimelineClip(Project& project, const std::string& clipId);
// clip の端のドラッグの種類。動かせる範囲の決め方が違う。
//   Trim   : 素材の範囲と 1 frame 以上の尺。left 端は timeline 先頭より前へ出さない
//   Ripple : Trim と同じだが、left 端でも clip の開始位置は動かない
//   Roll   : 操作した clip と接している clip の両方で素材の範囲と 1 frame 以上の尺
enum class EdgeEditKind { Trim, Ripple, Roll };

// 端のドラッグ量 (project frame) を、動かせる範囲で止めた値にする。Linked ならリンク相手も
// 同じ量を動かせる範囲に止める。Premiere と同じく、素材の端を越えるドラッグは失敗させずに
// 端で止める。trim / リップル / ローリングの確定と drag 中の表示の両方がこれを使う。
TimelineFrameResult clampEdgeEdit(const Project& project, const std::string& clipId, TrimEdge edge,
                                  EdgeEditKind kind, std::int64_t projectFrameDelta,
                                  LinkMode linkMode);

// Linked ならリンク相手の同じ側の端も同じ量だけ動かす。動かせる範囲を越える量は
// clampEdgeEdit で止め、1 frame も動かせなければ失敗する。
TimelineEditResult trimTimelineClip(Project& project, const std::string& clipId, TrimEdge edge,
                                    std::int64_t projectFrameDelta, LinkMode linkMode);
// clip の片端を timeline frame の位置 timelineFrame へ動かした clip (Project は変えない)。
// 素材範囲は trim・分割と同じ規則で決める。端がちょうどその位置に来ない (速度や fps の違いで
// 丸まる)、または素材の範囲を超えるなら nullopt。トランジションの描画区間を作るのに使う。
std::optional<TimelineClip> clipWithEdgeAt(const Project& project, const TimelineClip& clip,
                                           TrimEdge edge, std::int64_t timelineFrame,
                                           std::string& error);

// --- レート調整 (Premiere の Rate Stretch) --------------------------------
// 端をドラッグして、素材範囲 (in/out) を変えずに速度を変えて尺を伸縮する。反対側の端は動かさず、
// リップルも上書きもしない。Linked ならリンク相手にも同じ速度を適用し、同じ側の端を動かす
// (相手の速度がもともと違えば失敗する)。
//
// 尺 D にしたい clip の速度は s = (out - in) R / D (R = timeline fps / 素材 fps) とする。
// このとき実効 fps は (out - in) timeline fps / D で、尺はちょうど D になる。

// 端のドラッグ量 (project frame) を、伸縮できる範囲で止めた値にする。止める条件は
// 速度の範囲 (kMin/kMaxClipSpeedPercent)、1 frame 以上の尺、同じ track の隣の clip、
// timeline 先頭で、Linked ならリンク相手の条件も含める。確定と drag 中の表示の両方が使う。
TimelineFrameResult clampRateEdit(const Project& project, const std::string& clipId, TrimEdge edge,
                                  std::int64_t projectFrameDelta, LinkMode linkMode);

// drag 中の表示。clampRateEdit で止めた量で伸縮した結果を、対象 clip (Linked ならリンク相手も)
// ごとに返す。確定 (rateStretchTimelineClip) と同じ candidate から作るので、リンク相手の
// 尺が操作した clip と違っても、表示と確定が食い違わない (相手は同じ速度で自分の尺になる)。
struct RateStretchPreviewClip {
    std::string clipId;
    std::int64_t timelineStartFrame = 0;
    std::int64_t durationFrames = 0;
    std::int64_t speedNum = 1;
    std::int64_t speedDen = 1;
    // 開始 / 終端が現在の位置から動く frame 数 (drag 中の表示用)。
    std::int64_t startDelta = 0;
    std::int64_t endDelta = 0;
};

struct RateStretchPreview {
    bool success = false;
    // 操作した clip の端を実際に動かす量 (clampRateEdit の結果)。
    std::int64_t appliedDelta = 0;
    std::vector<RateStretchPreviewClip> clips;
    std::string error;
};

RateStretchPreview previewRateStretch(const Project& project, const std::string& clipId,
                                      TrimEdge edge, std::int64_t projectFrameDelta,
                                      LinkMode linkMode);
// clampRateEdit で止めた量で伸縮する。1 frame も伸縮できなければ失敗する。
TimelineEditResult rateStretchTimelineClip(Project& project, const std::string& clipId,
                                           TrimEdge edge, std::int64_t projectFrameDelta,
                                           LinkMode linkMode);

struct ClipSpeedDurationEdit {
    enum class Input { Speed, Duration } input = Input::Speed;
    std::int64_t speedNum = 1;
    std::int64_t speedDen = 1;
    std::int64_t durationFrames = 0;
    bool preservePitch = false;
    bool ripple = false;
    // ripple しないで延びた先にある clip を上書きする。開始位置は固定なので、重なるのは
    // 元の終端以降に始まる clip だけであり、丸ごと覆えば削除、はみ出せば左端を削る。
    bool overwrite = false;
};

struct ClipSpeedDurationPreview {
    bool success = false;
    std::int64_t durationFrames = 0;
    std::int64_t speedNum = 1;
    std::int64_t speedDen = 1;
    // ripple も overwrite もしないとき、延びた先の clip と重なるために失敗したか。
    // UI はこれを見て上書きの確認を出す。
    bool overlapsFollowing = false;
    std::string error;
};

ClipSpeedDurationPreview previewClipSpeedDuration(const Project& project, const std::string& clipId,
                                                  const ClipSpeedDurationEdit& edit,
                                                  LinkMode linkMode);
TimelineEditResult setClipSpeedDuration(Project& project, const std::string& clipId,
                                        const ClipSpeedDurationEdit& edit, LinkMode linkMode);
TimelineEditResult insertFrameHold(Project& project, const std::string& clipId, std::int64_t frame,
                                   std::int64_t holdFrames,
                                   const std::function<std::string()>& newId);

// --- Premiere 風の編集ツール ---------------------------------------------
// いずれも candidate 全体を validateTimeline で検証し、失敗時は Project を変更しない。

// frame で clip を 2 つに分割する (レーザーツール)。frame は clip の内側 (start < frame < end)
// でなければならない。Linked でリンク相手も frame を内側に含むなら同時に分割し、右半分どうしを
// newId() で作った新しい link group で結ぶ。片方だけを切ったら右半分は未リンクになる。
// フェードは左半分が in、右半分が out を引き継ぐ。
// selectedIndex は最初に指定した clip の左半分を指す。
TimelineEditResult splitTimelineClips(Project& project, const std::vector<std::string>& clipIds,
                                      std::int64_t frame, const std::function<std::string()>& newId,
                                      LinkMode linkMode);
// frame を内側に含む clip (全 track)。Shift+クリックの全 track 分割に使う。
std::vector<std::string> clipIdsSpanningFrame(const Project& project, std::int64_t frame);
// among のうち frame を内側 (start < frame < end) に含む clip。順序は among のまま。
// 存在しない ID は含めない。再生ヘッドでの分割 (Ctrl+K) に使う。
std::vector<std::string> clipIdsSpanningFrame(const Project& project, std::int64_t frame,
                                              const std::vector<std::string>& among);

// clip の音量を stepDb だけ上げ下げする (stepVolumePercentByDb の規則)。音量を持つのは audio
// clip だけなので、映像側はリンク相手の audio clip を変える。リンクの無い映像・文字・静止画は
// 対象外。音量 key があれば base と全 key を同じ規則で変え、0% の key (無音の形) は変えない。
// 対象が無い、または全対象が既に上限・下限で何も変わらないなら失敗し、Project を変えない。
TimelineEditResult stepClipVolume(Project& project, const std::vector<std::string>& clipIds,
                                  double stepDb);
// clip (とリンク相手) の有効/無効を切り換える (Shift+E)。対象に 1 つでも有効な clip があれば
// 全部を無効にし、全部が無効なら全部を有効にする (選択が混ざっていても 1 回で揃う)。
// 存在しない ID が含まれる、または clipIds が空なら失敗する。
TimelineEditResult toggleClipsEnabled(Project& project, const std::vector<std::string>& clipIds);

// 既定のトランジションの長さ (1 秒) を timeline frame で表した値。最低 1 frame。
std::int64_t defaultTransitionFrames(std::int64_t timelineFpsNum, std::int64_t timelineFpsDen);
// clip (とリンク相手) の先頭と末尾に timelineFrames の長さのフェードを付ける (Shift+D を clip
// 選択で押したとき)。フェードは素材 frame で持つので、clip ごとに速度込みで換算する。尺が
// 足りなければ前後で分け合う (先頭が半分を切り上げで取る)。トランジションのある端は変えない。
// 何も変わらなければ失敗し、Project を変えない。
TimelineEditResult applyDefaultClipFades(Project& project, const std::vector<std::string>& clipIds,
                                         std::int64_t timelineFrames);

// clip の edge 側で接している同じ track の clip の ID。無ければ空。
std::string touchingClipId(const Project& project, const std::string& clipId, TrimEdge edge);

struct TransitionEditResult {
    bool success = false;
    std::string transitionId; // outgoing / incoming に置いたトランジション
    std::int64_t frames = 0;  // 置いた長さ (余白が足りなければ求めた長さより短い)
    int transitionCount = 0;  // リンク相手の分を含めて置いた数
    std::string error;
};

// 編集点 (outgoing の終端 = incoming の先頭) に長さ timelineFrames の既定のトランジションを置く
// (Shift+D を編集点で押したとき)。cut を中央にし、片側の余白が足りなければもう片側へ寄せる。
// 素材 frame へ一意に換算できる長さになるまで縮める。0 frame になれば失敗する。
// その編集点の既存のトランジションは置き換え、両 clip のその端のフェードは消す。
// Linked で、リンク相手どうしも同じ cut で接していれば同じ長さで置く (映像と音声を一緒に)。
TransitionEditResult applyDefaultEditTransition(Project& project, const std::string& outgoingId,
                                                const std::string& incomingId,
                                                std::int64_t timelineFrames, LinkMode linkMode,
                                                const std::function<std::string()>& newId);
TimelineEditResult deleteTimelineTransition(Project& project, const std::string& transitionId);

struct TransitionSpanLimits {
    bool success = false;
    std::int64_t maxBefore = 0; // cut の前に置ける最大の長さ (timeline frame)
    std::int64_t maxAfter = 0;  // cut の後に置ける最大の長さ
    std::string error;
};

// 既存のトランジションの cut 前後に置ける長さの上限。素材の余白と、clip の反対側の端の
// トランジションが内側に使っていない分で決まる。Linked ならリンク相手の既存トランジションの
// 上限との小さい方。上限の内側でも、速度変更で素材 frame に乗らない長さや不透明度の下がる
// 区間は setTimelineTransitionSpan が断る。
TransitionSpanLimits transitionSpanLimits(const Project& project, const std::string& transitionId,
                                          LinkMode linkMode);

struct TransitionSpanFit {
    bool success = false;
    std::int64_t framesBeforeCut = 0;
    std::int64_t framesAfterCut = 0;
    std::string error;
    // frame ごとの不透明度を調べた回数と、長さごとに素材 frame へ乗るかを調べた回数。長尺素材で
    // 候補ごとに区間を辿り直さない (余白の長さに対して線形に留まる) ことの回帰検査に使う。
    std::uint64_t opacityProbes = 0;
    std::uint64_t edgeProbes = 0;
};

// 吸着で何を保つか。
//   EachSide : 前と後を独立に、それぞれ指定に最も近い値へ (片側の端のドラッグ。動かさない側は
//              今の値なので変わらない)
//   KeepTotal: 総尺 (前 + 後) を第一に保ち、その総尺の組のうち前が指定に最も近いもの (長さ・
//              配置の変更と本体のドラッグ)。その総尺で置けなければ最も近い総尺へ落とす
enum class SpanFitMode { EachSide, KeepTotal };

// 指定した cut 前後の長さに最も近い、置ける長さ (上限・素材 frame・不透明度を満たす)。
// 同じ距離なら今の値から離れる側 (変えようとした向き) を選ぶ。数値欄やドラッグの値を、素材
// frame に乗る長さへ吸着させるのに使う。setTimelineTransitionSpan 自体は丸めないので、吸着は
// 呼び出し側がこれで明示的に行う。
TransitionSpanFit nearestTransitionSpan(const Project& project, const std::string& transitionId,
                                        std::int64_t framesBeforeCut, std::int64_t framesAfterCut,
                                        SpanFitMode mode, LinkMode linkMode);
// 既存のトランジションの cut 前後の長さを変える (エフェクトコントロールの長さ・配置)。
// ID は変えない。Linked ならリンク相手の編集点の既存トランジションも同じ値にする (無ければ
// 作らない)。合計 1 frame 未満・上限超え・素材 frame に乗らない・不透明度が下がる・値が
// 変わらない場合は理由付きで失敗し、Project を変えない (置ける長さへ黙って丸めない)。
TransitionEditResult setTimelineTransitionSpan(Project& project, const std::string& transitionId,
                                               std::int64_t framesBeforeCut,
                                               std::int64_t framesAfterCut, LinkMode linkMode);

// リップルトリム。尺の増減を全トラックと字幕へ波及させる。
// left 端を trim しても clip の開始位置は動かない。
// Linked ならリンク相手も同じ量 trim し、相手の track の後ろもずらす。
TimelineEditResult rippleTrimTimelineClip(Project& project, const std::string& clipId,
                                          TrimEdge edge, std::int64_t projectFrameDelta,
                                          LinkMode linkMode);
// ローリング編集。clip の edge と接している隣の clip の境界を一緒に動かし、
// 2 clip の合計尺を保つ。接している clip が無ければ失敗する。
// Linked ならリンク相手の編集点も動かす。相手が編集点を持たない (L/J カット) なら動かさない。
TimelineEditResult rollTimelineEdit(Project& project, const std::string& clipId, TrimEdge edge,
                                    std::int64_t projectFrameDelta, LinkMode linkMode);
// スリップ。timeline 上の位置と素材上の長さを保ったまま in/out を同じだけずらす。
// 素材の範囲を超える分は素材の端で止める。1 frame もずらせなければ失敗する。
// Linked ならリンク相手も同じ量ずらし、全員がずらせる範囲で止める。
TimelineEditResult slipTimelineClip(Project& project, const std::string& clipId,
                                    std::int64_t projectFrameDelta, LinkMode linkMode);
// スライド。clip を尺を変えずに横へ動かし、接している前後の clip の out / in を
// 追従させる。操作した clip に接している前後の clip が無ければ失敗する (単なる移動に
// しない)。Linked ならリンク相手も同じようにスライドし、相手に接している clip があれば
// それも追従させる。
TimelineEditResult slideTimelineClip(Project& project, const std::string& clipId,
                                     std::int64_t projectFrameDelta, LinkMode linkMode);
// スライド量を、前の clip の out・後ろの clip の in を動かせる範囲 (素材の範囲と 1 frame 以上の
// 尺)、timeline 先頭、接していない clip との空白に止めた値にする。Linked ならリンク相手の
// 制約も含める。操作した clip に前後の clip が無ければ失敗する。
// slideTimelineClip の確定と drag 中の表示の両方がこれを使う。
TimelineFrameResult clampSlideEdit(const Project& project, const std::string& clipId,
                                   std::int64_t projectFrameDelta, LinkMode linkMode);

enum class SelectDirection { Forward, Backward };

// トラックの選択ツール。Forward は frame 以降に掛かる clip (end > frame)、Backward は
// frame 以前に掛かる clip (start <= frame)。track を指定するとその track だけに絞る。
std::vector<std::string> clipIdsFromFrame(const Project& project, std::int64_t frame,
                                          SelectDirection direction,
                                          std::optional<TrackRef> track = std::nullopt);
TimelineEditResult appendManimTimelineClipAt(Project& project, const ManimAsset& asset,
                                             std::string clipId, std::int64_t sourceFpsNum,
                                             std::int64_t sourceFpsDen,
                                             std::int64_t sourceFrameCount,
                                             std::int64_t timelineStartFrame, TrackRef track);

// 解像度とtimeline fpsを一つのtransactionで変更する。既存clipのsource domainは
// 変更せず、timelineStartFrameだけをwall-clock位置が保たれるよう新fpsへ換算する。
TimelineEditResult setProjectVideoSettings(Project& project, int width, int height,
                                           std::int64_t fpsNum, std::int64_t fpsDen);

// fpsだけを変更する互換入口。変換規則はsetProjectVideoSettingsと同じ。
TimelineEditResult setTimelineFrameRate(Project& project, std::int64_t fpsNum, std::int64_t fpsDen);

// track 編集。clip が載っている track は削除させない (暗黙に clip を消さない)。
TimelineEditResult addTrack(Project& project, TrackKind kind);
TimelineEditResult removeTrack(Project& project, TrackRef track);
TimelineEditResult setTrackMuted(Project& project, TrackRef track, bool muted);
// 同じ kind の複数 track の mute をまとめて変える (目玉のドラッグ塗り)。1 つでも不正な
// index があれば何も変えずに失敗する。
TimelineEditResult setTracksMuted(Project& project, TrackKind kind, const std::vector<int>& indices,
                                  bool muted);
// audio track だけが対象。video track を渡すと失敗する。
TimelineEditResult setTrackSolo(Project& project, TrackRef track, bool solo);

struct TimelineGap {
    bool found = false;
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::string error;
};

// track 上の timelineFrame を含む「clip が無く、後ろに clip がある」区間を返す。
// 終端より後ろの空白は詰める対象が無いので found=false とする。
TimelineGap gapAt(const Project& project, TrackRef track, std::int64_t timelineFrame);
// gapAt が返す区間を閉じ、後続 clip を左へ詰める (ripple delete)。
// 全トラックと字幕に同じ時間挿入・削除を適用する。区間をまたぐ素材は分割する。
TimelineEditResult editTimelineTime(Project& project, std::int64_t start, std::int64_t removed,
                                    std::int64_t inserted);
TimelineEditResult rippleDeleteGap(Project& project, TrackRef track, std::int64_t timelineFrame);

} // namespace mvm::project

#endif // MVM_PROJECT_TIMELINE_EDIT_H
