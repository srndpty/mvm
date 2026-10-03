// プロジェクトパネル (素材とフォルダ) の Project 層を検査する。
// 操作は失敗時に Project を変更しないこと、JSON は欠損・矛盾を既定値で埋めないことを見る。

#include "project/media_bin.h"
#include "project/path_identity.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

using mvm::project::MediaFolder;
using mvm::project::MediaItem;
using mvm::project::MediaKind;
using mvm::project::Project;

MediaItem videoItem(const char* id, const char* path, const char* folderId = "") {
    MediaItem item;
    item.id = id;
    item.kind = MediaKind::Video;
    item.mediaPath = path;
    item.name = path;
    item.folderId = folderId;
    item.fpsNum = 24000;
    item.fpsDen = 1001;
    item.frameCount = 598;
    item.width = 1920;
    item.height = 1080;
    return item;
}

MediaItem audioItem(const char* id, const char* path, const char* folderId = "") {
    MediaItem item;
    item.id = id;
    item.kind = MediaKind::Audio;
    item.mediaPath = path;
    item.name = path;
    item.folderId = folderId;
    item.sampleRate = 48000;
    item.durationSamples = 48000 * 10;
    return item;
}

MediaItem imageItem(const char* id, const char* path) {
    MediaItem item;
    item.id = id;
    item.kind = MediaKind::Image;
    item.mediaPath = path;
    item.name = path;
    item.width = 800;
    item.height = 600;
    return item;
}

// root/A/B と、B に video、root に audio を持つ Project。
Project nestedProject() {
    Project project = mvm::project::createDefaultProject();
    project.mediaFolders = {MediaFolder{"A", "A", ""}, MediaFolder{"B", "B", "A"}};
    project.mediaItems = {videoItem("v", "C:/media/v.mp4", "B"), audioItem("a", "C:/media/a.wav")};
    return project;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    check(output.good(), "テストJSONを書き込めません");
}

void testEdits() {
    Project project = nestedProject();
    check(mvm::project::validateMediaBin(project).success, "正しい入れ子のbinを拒否しました");

    const auto added = mvm::project::addMediaFolder(project, MediaFolder{"C", "C", "B"});
    check(added.success && project.mediaFolders.size() == 3, "入れ子のfolderを追加できません");

    // 自分の子孫へ移すと循環する。失敗時は Project を変えない。
    const Project beforeCycle = project;
    check(!mvm::project::moveMediaBinEntries(project, {"A"}, "C").success,
          "folderを自分の子孫へ移動できてしまいます");
    check(!mvm::project::moveMediaBinEntries(project, {"A"}, "A").success,
          "folderを自分自身へ移動できてしまいます");
    check(project == beforeCycle, "失敗した移動でProjectが変化しました");
    check(!mvm::project::moveMediaBinEntries(project, {"v"}, "missing").success,
          "存在しないfolderへ移動できてしまいます");
    check(!mvm::project::moveMediaBinEntries(project, {"missing"}, "").success,
          "存在しないentryの移動が成功しました");

    check(mvm::project::moveMediaBinEntries(project, {"v", "C"}, "").success &&
              mvm::project::findMediaItem(project, "v")->folderId.empty() &&
              mvm::project::findMediaFolder(project, "C")->parentId.empty(),
          "素材とfolderをまとめてrootへ移動できません");

    check(!mvm::project::renameMediaBinEntry(project, "A", "").success,
          "空の名前へ変更できてしまいます");
    check(mvm::project::renameMediaBinEntry(project, "a", "ナレーション").success &&
              mvm::project::findMediaItem(project, "a")->name == "ナレーション",
          "素材の名前を変更できません");

    // 同じファイルの素材は 2 つ持たない。表記ゆれ (..) も同じファイルとして扱う。
    const auto duplicate =
        mvm::project::addMediaItem(project, audioItem("a2", "C:/media/x/../a.wav"));
    check(!duplicate.success && project.mediaItems.size() == 2,
          "同じファイルの素材を重複して追加しました");
    check(mvm::project::addMediaItem(project, imageItem("i", "C:/media/still.png")).success,
          "静止画素材を追加できません");

    // 調べ直した値で技術的な値だけを置き換える。id・名前・フォルダ・ファイルは変えない。
    check(mvm::project::moveMediaBinEntries(project, {"i"}, "A").success &&
              mvm::project::renameMediaBinEntry(project, "i", "表紙").success,
          "前提: 画像素材をフォルダへ移して名前を変えられません");
    auto portrait = imageItem("other-id", "C:/elsewhere/other.png");
    portrait.width = 600;
    portrait.height = 800;
    check(mvm::project::refreshMediaItem(project, "i", portrait).success,
          "調べ直した値で素材を更新できません");
    const auto* refreshed = mvm::project::findMediaItem(project, "i");
    check(refreshed && refreshed->width == 600 && refreshed->height == 800 &&
              refreshed->name == "表紙" && refreshed->folderId == "A" &&
              refreshed->mediaPath == "C:/media/still.png",
          "素材の更新で技術的な値以外が変わった、または値が更新されません");
    const Project beforeRefresh = project;
    auto broken = portrait;
    broken.width = 0;
    check(!mvm::project::refreshMediaItem(project, "i", broken).success && project == beforeRefresh,
          "不正な値で素材を更新できてしまいます");
    check(!mvm::project::refreshMediaItem(project, "missing", portrait).success &&
              project == beforeRefresh,
          "存在しない素材を更新できてしまいます");
}

mvm::project::TimelineClip timelineClip(const char* id, mvm::project::TimelineClipKind kind,
                                        const std::filesystem::path& path,
                                        mvm::project::TrackKind track, std::int64_t start) {
    mvm::project::TimelineClip clip;
    clip.kind = kind;
    clip.mediaPath = path;
    clip.name = id;
    clip.id = id;
    clip.sourceFpsNum = 60;
    clip.sourceFpsDen = 1;
    clip.sourceFrameCount = 60;
    clip.sourceInFrame = 0;
    clip.sourceOutFrame = 60;
    clip.timelineStartFrame = start;
    clip.track = {track, 0};
    return clip;
}

// v.mp4 のリンク対 (映像と音声)、a.wav の音声、文字を timeline に置いた Project。
Project usedProject() {
    using mvm::project::TimelineClipKind;
    using mvm::project::TrackKind;
    Project project = nestedProject();
    auto video =
        timelineClip("clip-v", TimelineClipKind::Video, "C:/media/v.mp4", TrackKind::Video, 0);
    auto videoAudio = timelineClip("clip-v-audio", TimelineClipKind::Audio, "C:/media/v.mp4",
                                   TrackKind::Audio, 0);
    video.linkGroupId = videoAudio.linkGroupId = "pair";
    video.mediaItemId = videoAudio.mediaItemId = "v";
    auto audio =
        timelineClip("clip-a", TimelineClipKind::Audio, "C:/media/a.wav", TrackKind::Audio, 100);
    audio.mediaItemId = "a";
    auto text = timelineClip("clip-text", TimelineClipKind::Text, {}, TrackKind::Video, 100);
    text.text.content = "字幕";
    project.timelineClips = {video, videoAudio, audio, text};
    return project;
}

bool hasClip(const Project& project, const char* id) {
    for (const auto& clip : project.timelineClips) {
        if (clip.id == id)
            return true;
    }
    return false;
}

void testRemove() {
    Project project = nestedProject();
    // A を消すと、子孫の B とその中の素材も消える。root の素材は残る。
    check(mvm::project::removeMediaBinEntries(project, {"A"}).success, "folderを削除できません");
    check(project.mediaFolders.empty() && project.mediaItems.size() == 1 &&
              project.mediaItems.front().id == "a",
          "folder削除で子孫が残る、または無関係な素材が消えました");

    // 使用中の素材は、それを参照する clip (とリンク相手) ごと消える。
    Project used = usedProject();
    check(mvm::project::validateTimeline(used).success &&
              mvm::project::validateMediaBin(used).success,
          "使用中素材の試験用Projectが不正です");
    check(mvm::project::mediaItemsInUse(used) == std::set<std::string>{"v", "a"},
          "使用中の素材の判定が違います");
    const auto plan = mvm::project::planMediaBinRemoval(used, {"v"});
    check(plan.success && plan.clipIds == std::vector<std::string>{"clip-v", "clip-v-audio"},
          "使用中素材の削除で消えるclipの見積もりが違います");
    const auto removed = mvm::project::removeMediaBinEntries(used, {"v"});
    check(removed.success && removed.removedClipIds == plan.clipIds,
          "使用中素材をclipごと削除できません");
    check(!mvm::project::findMediaItem(used, "v") && !hasClip(used, "clip-v") &&
              !hasClip(used, "clip-v-audio"),
          "削除した素材を参照するclipが残りました");
    // 対照: 別素材の clip と、ファイルを参照しない文字 clip は残る。
    check(hasClip(used, "clip-a") && hasClip(used, "clip-text") &&
              mvm::project::findMediaItem(used, "a"),
          "削除した素材と無関係なclipや素材が消えました");

    // folder の削除は、その中の素材を参照する clip も消す。
    Project viaFolder = usedProject();
    const auto folderRemoved = mvm::project::removeMediaBinEntries(viaFolder, {"A"});
    check(folderRemoved.success && folderRemoved.removedClipIds.size() == 2 &&
              !hasClip(viaFolder, "clip-v") && hasClip(viaFolder, "clip-a"),
          "folder削除で中の素材を参照するclipを削除できません");

    // 未使用の素材なら clip は消えない。
    Project unused = usedProject();
    unused.timelineClips.erase(unused.timelineClips.begin(), unused.timelineClips.begin() + 2);
    const auto unusedPlan = mvm::project::planMediaBinRemoval(unused, {"v"});
    check(unusedPlan.success && unusedPlan.clipIds.empty(),
          "未使用の素材の削除でclipを巻き込む見積もりになりました");

    // 失敗時は Project を変えない。
    Project invalid = usedProject();
    const Project beforeInvalid = invalid;
    check(!mvm::project::removeMediaBinEntries(invalid, {"v", "missing"}).success &&
              invalid == beforeInvalid,
          "存在しないentryを含む削除で一部だけ消えました");
    check(!mvm::project::removeMediaBinEntries(invalid, {}).success && invalid == beforeInvalid,
          "空の削除が成功しました");
    check(!mvm::project::planMediaBinRemoval(invalid, {"missing"}).success,
          "存在しないentryの削除見積もりが成功しました");
}

// 使っている素材の時間軸が外部で変わったときは、素材だけを更新しない。
void testRefreshTiming() {
    // timeline で使っている動画の時間軸 (fps・尺・種類) が変わった更新は拒否する。
    // clip の素材範囲は元の時間軸で決めてあり、素材だけ新しくすると食い違う。
    Project used = usedProject(); // v.mp4 (24000/1001, 598 frame) をリンク対が使う
    const Project beforeTiming = used;
    auto retimed = *mvm::project::findMediaItem(used, "v");
    retimed.fpsNum = 30;
    retimed.fpsDen = 1;
    retimed.frameCount = 120;
    check(!mvm::project::refreshMediaItem(used, "v", retimed).success && used == beforeTiming,
          "使用中の動画の fps・尺の変化で素材だけを更新しました");
    auto recast = *mvm::project::findMediaItem(used, "a");
    recast.durationSamples = 48000;
    check(!mvm::project::refreshMediaItem(used, "a", recast).success && used == beforeTiming,
          "使用中の音声の尺の変化で素材だけを更新しました");
    // 解像度だけなら clip の時間軸に関わらないので更新する。
    auto resized = *mvm::project::findMediaItem(used, "v");
    resized.width = 1280;
    resized.height = 720;
    check(mvm::project::refreshMediaItem(used, "v", resized).success &&
              mvm::project::findMediaItem(used, "v")->width == 1280,
          "使用中の動画の解像度だけの変化を更新できません");
    // 使われていない素材なら時間軸が変わっても更新する。
    Project unusedMedia = usedProject();
    unusedMedia.timelineClips.erase(unusedMedia.timelineClips.begin(),
                                    unusedMedia.timelineClips.begin() + 2);
    check(mvm::project::refreshMediaItem(unusedMedia, "v", retimed).success &&
              mvm::project::findMediaItem(unusedMedia, "v")->fpsNum == 30,
          "使われていない素材の時間軸の変化を更新できません");
}

void testValidation() {
    // 種別ごとに意味の無い値を持つ素材は拒否する。
    Project audioWithSize = nestedProject();
    audioWithSize.mediaItems[1].width = 640;
    check(!mvm::project::validateMediaBin(audioWithSize).success,
          "解像度を持つ音声素材を受理しました");

    Project videoWithoutFps = nestedProject();
    videoWithoutFps.mediaItems[0].fpsNum = 0;
    check(!mvm::project::validateMediaBin(videoWithoutFps).success,
          "fpsの無い動画素材を受理しました");

    Project nonCanonicalFps = nestedProject();
    nonCanonicalFps.mediaItems[0].fpsNum = 120;
    nonCanonicalFps.mediaItems[0].fpsDen = 2;
    check(!mvm::project::validateMediaBin(nonCanonicalFps).success,
          "約分されていないfpsを受理しました");

    Project imageWithDuration = nestedProject();
    imageWithDuration.mediaItems.push_back(imageItem("i", "C:/media/still.png"));
    imageWithDuration.mediaItems.back().frameCount = 10;
    check(!mvm::project::validateMediaBin(imageWithDuration).success,
          "尺を持つ静止画素材を受理しました");

    Project cycle = nestedProject();
    cycle.mediaFolders[0].parentId = "B"; // A -> B -> A
    check(!mvm::project::validateMediaBin(cycle).success, "循環したfolderを受理しました");

    Project orphan = nestedProject();
    orphan.mediaItems[0].folderId = "missing";
    check(!mvm::project::validateMediaBin(orphan).success,
          "存在しないfolderに属する素材を受理しました");

    // folder と item は同じ id 名前空間を共有する。UI は id だけで entry を指すため。
    Project sharedId = nestedProject();
    sharedId.mediaItems[1].id = "A";
    check(!mvm::project::validateMediaBin(sharedId).success, "folderと同じidの素材を受理しました");
}

// Windows では大文字小文字違い・hard link は同じ実体を指す。lexically_normal だけで
// 比べると別素材として二重に登録でき、使用中判定もすり抜ける。
void testFileIdentity(const std::filesystem::path& root) {
    const auto directory = root / "identity";
    std::filesystem::create_directories(directory);
    const auto original = directory / "Voice.wav";
    writeText(original, "RIFF");
    const auto link = directory / "voice-link.wav";
    std::error_code linkError;
    std::filesystem::create_hard_link(original, link, linkError);
    check(!linkError, "hard link を作れません (NTFS 以外で実行していないか確認してください)");

    Project project = mvm::project::createDefaultProject();
    check(mvm::project::addMediaItem(project, audioItem("a", "placeholder")).success,
          "識別試験の素材を追加できません");
    project.mediaItems.front().mediaPath = original;

    const auto upper = directory / "VOICE.WAV";
    check(mvm::project::findMediaItemByPath(project, upper) != nullptr,
          "大文字小文字だけが違うpathを別の素材として扱いました");
    check(mvm::project::findMediaItemByPath(project, link) != nullptr,
          "hard linkを別の素材として扱いました");
    MediaItem viaLink = audioItem("c", "x");
    viaLink.mediaPath = link;
    check(!mvm::project::addMediaItem(project, viaLink).success && project.mediaItems.size() == 1,
          "hard link経由で同じファイルを二重に登録できてしまいます");

    // 対照: 別ファイルは別素材。
    const auto other = directory / "other.wav";
    writeText(other, "RIFF");
    check(mvm::project::findMediaItemByPath(project, other) == nullptr,
          "別のファイルを同じ素材として扱いました");

    // canonical Project の比較も実体で行う (recovery の foreign 判定・Save As が使う)。
    check(mvm::project::sameCanonicalPath(original, link),
          "hard linkのProject pathを別物と判定しました");
    check(!mvm::project::sameCanonicalPath(original, other),
          "別ファイルのProject pathを同じと判定しました");

    // 存在しないファイルは表記で比べる。区切り文字と .. は揃える。
    Project missing = mvm::project::createDefaultProject();
    missing.mediaItems = {audioItem("m1", "C:/mvm-missing/Tone.wav"),
                          audioItem("m2", R"(C:\mvm-missing\x\..\Tone.wav)")};
    check(!mvm::project::validateMediaBin(missing).success,
          "区切り文字と..だけが違う素材の重複を受理しました");
    // 大文字小文字は畳まない。case-sensitive directory では別のファイルでありうるので、
    // 重複として Project を開けなくしない。
    Project caseOnly = mvm::project::createDefaultProject();
    caseOnly.mediaItems = {audioItem("m1", "C:/mvm-missing/Tone.wav"),
                           audioItem("m2", "C:/mvm-missing/tone.wav")};
    check(mvm::project::validateMediaBin(caseOnly).success,
          "大文字小文字だけが違う素材を重複として拒否しました");
}

// 実体 (file ID) が取れているときは表記より実体で決める。case-sensitive directory の
// A.mp4 と a.mp4 は、大文字小文字を畳んだ表記が同じでも file ID が違う。実 directory の
// case sensitivity は権限と機能の有無で作れない環境があるので、key を直接組み立てる。
void testCaseSensitiveIdentity() {
    using mvm::project::FileIdentityKey;
    using mvm::project::FileIdentityStatus;
    using mvm::project::PathSameness;
    const FileIdentityKey upper{FileIdentityStatus::FileId, L"C:/case-sensitive/A.mp4", L"1:aa"};
    const FileIdentityKey lower{FileIdentityStatus::FileId, L"C:/case-sensitive/a.mp4", L"1:bb"};
    check(mvm::project::comparePathIdentity(upper, lower) == PathSameness::Different,
          "file IDの違う大文字小文字違いのpathを同じと判定しました");
    // 表記が同じでも file ID が違えば別物 (表記で先に Same を返さない)。
    const FileIdentityKey replaced{FileIdentityStatus::FileId, upper.pathKey, L"1:cc"};
    check(mvm::project::comparePathIdentity(upper, replaced) == PathSameness::Different,
          "表記が同じでfile IDの違うpathを同じと判定しました");
    // 対照: file ID が同じなら表記が違っても同じ実体。
    const FileIdentityKey alias{FileIdentityStatus::FileId, L"C:/case-sensitive/x.mp4", L"1:aa"};
    check(mvm::project::comparePathIdentity(upper, alias) == PathSameness::Same,
          "file IDの同じpathを別物と判定しました");
    check(mvm::project::canonicalPathKey("C:/case-sensitive/A.mp4") !=
              mvm::project::canonicalPathKey("C:/case-sensitive/a.mp4"),
          "表記上のkeyが大文字小文字を畳んでいます");
}

// identity を取れなかったこと (Unavailable) を「存在しない」と同じに扱わない。
// 同じにすると、同じ実体でも表記が違えば別物と判定し、使用中の素材を削除できてしまう。
void testUnavailableIdentity(const std::filesystem::path& root) {
    using mvm::project::FileIdentityStatus;
    using mvm::project::PathSameness;
    const auto directory = root / "unavailable";
    std::filesystem::create_directories(directory);
    const auto file = directory / "clip.wav";
    writeText(file, "RIFF");
    const auto missing = directory / "missing.wav";

    // 状態の分類。ディレクトリは「存在するが通常ファイルの identity を取れない」例。
    check(mvm::project::fileIdentityKey(file).status == FileIdentityStatus::FileId,
          "通常ファイルのidentityを取れません");
    check(mvm::project::fileIdentityKey(missing).status == FileIdentityStatus::Missing,
          "存在しないpathをMissingと分類しません");
    check(mvm::project::fileIdentityKey(directory).status == FileIdentityStatus::Unavailable,
          "identityを取れないpathをUnavailableと分類しません");

    check(mvm::project::comparePathIdentity(file, missing) == PathSameness::Different,
          "存在するファイルと存在しないpathを別物と判定しません");
    check(mvm::project::comparePathIdentity(file, directory) == PathSameness::Unknown,
          "identityを取れないpathとの比較をUnknownにしません");
    check(mvm::project::comparePathIdentity(directory, root / "x" / ".." / "unavailable") ==
              PathSameness::Same,
          "表記が同じならidentityを取れなくても同じと判定しません");
    // 大文字小文字だけが違う表記は、identity を取れなければ同じとも違うとも言えない。
    check(mvm::project::comparePathIdentity(directory, root / "UNAVAILABLE") ==
              PathSameness::Unknown,
          "identityを取れない大文字小文字違いのpathをUnknownにしません");

    // 素材の重複登録の判定でも、identity を取れない相手を同じ素材とみなさない。
    Project project = mvm::project::createDefaultProject();
    MediaItem item = audioItem("u", "x");
    item.mediaPath = directory;
    project.mediaItems.push_back(item);
    check(mvm::project::findMediaItemByPath(project, file) == nullptr,
          "identityを取れない素材を同じ素材とみなしました");
}

// 動画・音声・画像の clip はプロジェクトパネルの素材を mediaItemId で指す。
// 素材の無い clip・種別やファイルの食い違い・文字の素材参照を拒否する。
void testMediaReferences() {
    using mvm::project::TimelineClipKind;
    const Project control = usedProject();
    check(mvm::project::validateMediaReferences(control).success,
          "正しい素材参照を拒否しました (対照群)");

    Project orphan = control;
    orphan.timelineClips[2].mediaItemId.clear();
    check(!mvm::project::validateMediaReferences(orphan).success,
          "素材を指さない音声clipを受理しました");
    Project missing = control;
    missing.timelineClips[2].mediaItemId = "missing";
    check(!mvm::project::validateMediaBin(missing).success,
          "プロジェクトパネルに無い素材を指すclipを受理しました");
    Project wrongKind = control;
    wrongKind.timelineClips[0].mediaItemId = "a"; // 動画 clip が音声素材を指す
    wrongKind.timelineClips[0].mediaPath = "C:/media/a.wav";
    check(!mvm::project::validateMediaReferences(wrongKind).success,
          "種別の違う素材を指すclipを受理しました");
    Project wrongFile = control;
    wrongFile.timelineClips[2].mediaPath = "C:/media/other.wav";
    check(!mvm::project::validateMediaReferences(wrongFile).success,
          "素材と違うファイルを指すclipを受理しました");
    // 表記ゆれ (区切り文字・..) は同じファイル。
    Project spelling = control;
    spelling.timelineClips[2].mediaPath = R"(C:\media\x\..\a.wav)";
    check(mvm::project::validateMediaReferences(spelling).success,
          "表記だけが違う同じファイルを拒否しました");
    // 大文字小文字の違いは I/O なしでは同じと言えない (case-sensitive directory では別ファイル)。
    Project caseOnly = control;
    caseOnly.timelineClips[2].mediaPath = "C:/MEDIA/A.WAV";
    check(!mvm::project::validateMediaReferences(caseOnly).success,
          "大文字小文字だけが違うpathを素材と同じファイルとして受理しました");
    Project textWithItem = control;
    textWithItem.timelineClips[3].mediaItemId = "a";
    check(!mvm::project::validateMediaReferences(textWithItem).success,
          "文字clipが素材を指しています");

    // 保存も読み込みも同じ規則を通す。
    const auto path = std::filesystem::temp_directory_path() / "mvm-media-reference-orphan.mvm";
    check(!mvm::project::saveProjectJson(missing, path).success,
          "素材の無いclipを持つProjectを保存できてしまいます");

    // media_item_id は JSON を往復する。欠けた clip は既定値で埋めずに拒否する。
    const auto roundTrip = std::filesystem::temp_directory_path() / "mvm-media-reference.mvm";
    check(mvm::project::saveProjectJson(control, roundTrip).success,
          "素材参照を持つProjectを保存できません");
    const auto loaded = mvm::project::loadProjectJson(roundTrip);
    check(loaded.success && loaded.project.timelineClips.size() == control.timelineClips.size() &&
              loaded.project.timelineClips[0].mediaItemId == "v" &&
              loaded.project.timelineClips[1].mediaItemId == "v" &&
              loaded.project.timelineClips[2].mediaItemId == "a" &&
              loaded.project.timelineClips[3].mediaItemId.empty(),
          "media_item_idがJSON round-tripしません");
    std::ifstream saved(roundTrip, std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
    saved.close();
    const std::string field = "\"media_item_id\": \"a\",";
    const auto fieldAt = json.find(field);
    check(fieldAt != std::string::npos, "保存JSONにmedia_item_idが書かれていません");
    if (fieldAt != std::string::npos) {
        json.erase(fieldAt, field.size());
        writeText(roundTrip, json);
        check(!mvm::project::loadProjectJson(roundTrip).success,
              "media_item_idの無いclipを読み込めてしまいます");
    }
    std::filesystem::remove(roundTrip);

    // 以前の版 (同じ schema 14) は clip と素材の path を大文字小文字を畳んで照合していた。
    // 大文字小文字だけが違う Project は、読み込むときに clip の path を素材の表記へ揃えて開く。
    // 大文字小文字以外も違う path (別のファイル) は従来どおり拒否する。
    const auto legacyCase = std::filesystem::temp_directory_path() / "mvm-media-reference-case.mvm";
    check(mvm::project::saveProjectJson(control, legacyCase).success,
          "前提: 大文字小文字の移行試験の Project を保存できません");
    std::ifstream legacySaved(legacyCase, std::ios::binary);
    const std::string legacyJson((std::istreambuf_iterator<char>(legacySaved)),
                                 std::istreambuf_iterator<char>());
    legacySaved.close();
    // project の外の素材も同じ drive なら relative で書かれるので、末尾で探す。
    const std::string spelled = "media/a.wav";
    const auto spelledAt = legacyJson.find(spelled);
    check(spelledAt != std::string::npos &&
              legacyJson.find(spelled, spelledAt + 1) != std::string::npos,
          "前提: 保存JSONに素材と clip の path がありません");
    if (spelledAt != std::string::npos) {
        // 先に現れる方 (素材か clip のどちらか) だけを大文字にする。
        std::string upper = legacyJson;
        upper.replace(spelledAt, spelled.size(), "MEDIA/A.WAV");
        writeText(legacyCase, upper);
        const auto migrated = mvm::project::loadProjectJson(legacyCase);
        const auto* item =
            migrated.success ? mvm::project::findMediaItem(migrated.project, "a") : nullptr;
        check(migrated.success && item &&
                  migrated.project.timelineClips[2].mediaPath == item->mediaPath,
              "大文字小文字だけが素材と違う schema 14 の clip を素材の表記へ揃えて開けません");
        std::string other = legacyJson;
        other.replace(spelledAt, spelled.size(), "MEDIA/B.WAV");
        writeText(legacyCase, other);
        check(!mvm::project::loadProjectJson(legacyCase).success,
              "素材と違うファイルを指す clip を読み込み時に素材の path へ書き換えました");
    }
    std::filesystem::remove(legacyCase);

    // 揃えてよいかは実体で決める。case-sensitive directory は権限と機能の有無で作れない環境が
    // あるので、file ID の違いは組み立てた FileIdentityKey で見る。
    using mvm::project::FileIdentityKey;
    using mvm::project::FileIdentityStatus;
    using mvm::project::mayAdoptLegacyCaseSpelling;
    const FileIdentityKey fileX{FileIdentityStatus::FileId, L"C:/case/a.mp4", L"1:x"};
    const FileIdentityKey fileXUpper{FileIdentityStatus::FileId, L"C:/case/A.mp4", L"1:x"};
    const FileIdentityKey fileY{FileIdentityStatus::FileId, L"C:/case/A.mp4", L"1:y"};
    const FileIdentityKey missingUpper{FileIdentityStatus::Missing, L"C:/case/A.mp4", {}};
    const FileIdentityKey missingLower{FileIdentityStatus::Missing, L"C:/case/a.mp4", {}};
    const FileIdentityKey unavailable{FileIdentityStatus::Unavailable, L"C:/case/A.mp4", {}};
    check(mayAdoptLegacyCaseSpelling(fileXUpper, fileX),
          "同じ file ID の大文字小文字違いを素材の表記へ揃えません");
    check(!mayAdoptLegacyCaseSpelling(fileY, fileX),
          "file ID の違う (case-sensitive directory の別ファイル) clip を素材の表記へ揃えました");
    check(mayAdoptLegacyCaseSpelling(missingUpper, missingLower),
          "どちらも無いファイルの大文字小文字違いを素材の表記へ揃えません");
    check(!mayAdoptLegacyCaseSpelling(missingUpper, fileX) &&
              !mayAdoptLegacyCaseSpelling(fileXUpper, missingLower),
          "片方だけ実在する clip を素材の表記へ揃えました");
    check(!mayAdoptLegacyCaseSpelling(unavailable, fileX) &&
              !mayAdoptLegacyCaseSpelling(fileX, unavailable),
          "実体を確かめられない clip を素材の表記へ揃えました");
}

// 実在するファイルで、大文字小文字だけが違う clip を読み込み時に素材の表記へ揃える
// (case-insensitive な通常の directory では同じ file ID になる)。
void testLegacyCaseSpellingOnDisk(const std::filesystem::path& root) {
    const auto media = root / "legacy-case";
    std::filesystem::create_directories(media);
    const auto wav = media / "Voice.wav";
    writeText(wav, "RIFF");
    Project project = usedProject();
    for (auto& item : project.mediaItems)
        if (item.id == "a")
            item.mediaPath = wav;
    project.timelineClips[2].mediaPath = wav;
    const auto path = root / "legacy-case.mvm";
    check(mvm::project::saveProjectJson(project, path).success,
          "前提: 実ファイルの大文字小文字の試験の Project を保存できません");
    std::ifstream saved(path, std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
    saved.close();
    const std::string spelled = "legacy-case/Voice.wav";
    const auto at = json.find(spelled);
    check(at != std::string::npos, "前提: 保存JSONに素材の path がありません");
    if (at == std::string::npos)
        return;
    json.replace(at, spelled.size(), "legacy-case/VOICE.WAV");
    writeText(path, json);
    const auto loaded = mvm::project::loadProjectJson(path);
    const auto* item = loaded.success ? mvm::project::findMediaItem(loaded.project, "a") : nullptr;
    check(loaded.success && item && loaded.project.timelineClips[2].mediaPath == item->mediaPath,
          "同じ実ファイルを指す大文字小文字違いの clip を素材の表記へ揃えて開けません");
}

void testJson(const std::filesystem::path& root) {
    Project project = nestedProject();
    project.mediaItems[0].mediaPath = root / "media" / "v.mp4";
    project.mediaItems[1].mediaPath = "C:/elsewhere/a.wav";
    project.mediaItems.push_back(imageItem("i", "C:/elsewhere/still.png"));
    const auto path = root / "bin.mvm";
    check(mvm::project::saveProjectJson(project, path).success, "binを持つProjectを保存できません");
    const auto loaded = mvm::project::loadProjectJson(path);
    check(loaded.success, "保存したbinを読み込めません");
    check(loaded.success && loaded.project.mediaFolders == project.mediaFolders,
          "folderがJSON round-tripしません");
    bool itemsMatch = loaded.success && loaded.project.mediaItems.size() == 3;
    for (std::size_t index = 0; itemsMatch && index < 3; ++index) {
        auto expected = project.mediaItems[index];
        expected.mediaPath = expected.mediaPath.lexically_normal();
        itemsMatch = loaded.project.mediaItems[index] == expected;
    }
    check(itemsMatch, "素材の種別・path・値がJSON round-tripしません");

    // 保存形式の検査: project 配下の素材は relative で書かれる。
    std::ifstream saved(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(saved), std::istreambuf_iterator<char>()};
    check(text.find("\"media_path\": \"media/v.mp4\"") != std::string::npos,
          "project配下の素材pathがrelativeで保存されていません");

    const std::string header =
        R"JSON({"schema_version":15,"timeline_markers":[],"timeline_transitions":[],"in_frame":null,"out_frame":null,"format":"mvm-project","timeline_fps_num":60,"timeline_fps_den":1,)JSON"
        R"JSON("video_tracks":[{"name":"V1","muted":false,"solo":false}],"audio_tracks":[],"manim_assets":[],)JSON"
        R"JSON("timeline_clips":[],"media_folders":[],)JSON";
    const std::string item =
        R"JSON({"id":"a","kind":"audio","media_path":"a.wav","name":"a.wav","folder_id":"",)JSON"
        R"JSON("fps_num":0,"fps_den":1,"frame_count":0,"width":0,"height":0,"sample_rate":48000,)JSON";

    // 対照群。これが通るので、以下の negative は壊した箇所で落ちている。
    const auto good = root / "good.mvm";
    writeText(good, header + R"JSON("media_items":[)JSON" + item +
                        R"JSON("duration_samples":480000}]})JSON");
    check(mvm::project::loadProjectJson(good).success, "正しいmedia_itemsを読めません (対照群)");

    const auto missingItems = root / "missing-items.mvm";
    writeText(missingItems, header.substr(0, header.size() - 1) + "}");
    check(!mvm::project::loadProjectJson(missingItems).success,
          "media_itemsの無いProjectを受理しました");

    const auto missingField = root / "missing-field.mvm";
    writeText(missingField,
              header + R"JSON("media_items":[)JSON" + item.substr(0, item.size() - 1) + "}]}");
    check(!mvm::project::loadProjectJson(missingField).success,
          "duration_samplesの無い素材を既定値で受理しました");

    const auto unknownKind = root / "unknown-kind.mvm";
    std::string unknown = item;
    unknown.replace(unknown.find("\"audio\""), 7, "\"midi\"");
    writeText(unknownKind, header + R"JSON("media_items":[)JSON" + unknown +
                               R"JSON("duration_samples":480000}]})JSON");
    check(!mvm::project::loadProjectJson(unknownKind).success, "未知の素材種別を受理しました");

    const auto zeroDuration = root / "zero-duration.mvm";
    writeText(zeroDuration,
              header + R"JSON("media_items":[)JSON" + item + R"JSON("duration_samples":0}]})JSON");
    check(!mvm::project::loadProjectJson(zeroDuration).success, "尺0の音声素材を受理しました");

    // binを持たない schema 3 は移行せずに拒否する。
    const auto schema3 = root / "schema3.mvm";
    writeText(
        schema3,
        R"JSON({"schema_version":3,"format":"mvm-project","timeline_fps_num":60,)JSON"
        R"JSON("timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false,"solo":false}],)JSON"
        R"JSON("audio_tracks":[],"manim_assets":[],"timeline_clips":[]})JSON");
    check(!mvm::project::loadProjectJson(schema3).success, "schema 3のProjectを受理しました");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: mvm_test_media_bin <work-directory>\n");
        return 2;
    }
    const std::filesystem::path root = std::filesystem::absolute(argv[1]).lexically_normal();
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    testEdits();
    testRemove();
    testValidation();
    testFileIdentity(root);
    testMediaReferences();
    testRefreshTiming();
    testUnavailableIdentity(root);
    testCaseSensitiveIdentity();
    testJson(root);
    testLegacyCaseSpellingOnDisk(root);

    if (failures == 0)
        std::printf("media bin: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
