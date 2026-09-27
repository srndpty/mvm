// プロジェクトパネル (素材とフォルダ) の Project 層を検査する。
// 操作は失敗時に Project を変更しないこと、JSON は欠損・矛盾を既定値で埋めないことを見る。

#include "project/media_bin.h"
#include "project/path_identity.h"
#include "project/project_json.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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
}

void testRemove() {
    Project project = nestedProject();
    // A を消すと、子孫の B とその中の素材も消える。root の素材は残る。
    check(mvm::project::removeMediaBinEntries(project, {"A"}).success, "folderを削除できません");
    check(project.mediaFolders.empty() && project.mediaItems.size() == 1 &&
              project.mediaItems.front().id == "a",
          "folder削除で子孫が残る、または無関係な素材が消えました");

    // timeline が使っている素材を含む削除は、全体を拒否する。
    Project used = nestedProject();
    mvm::project::TimelineClip clip;
    clip.mediaPath = "C:/media/v.mp4";
    used.timelineClips.push_back(clip);
    const Project beforeRemove = used;
    check(!mvm::project::removeMediaBinEntries(used, {"A", "a"}).success,
          "timelineで使用中の素材を含むfolderを削除できてしまいます");
    check(used == beforeRemove, "拒否した削除で一部だけ消えました");
    check(!mvm::project::removeMediaBinEntries(used, {"missing"}).success,
          "存在しないentryの削除が成功しました");
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

    // timeline が hard link 経由で参照していても使用中とみなし、削除を拒否する。
    mvm::project::TimelineClip clip;
    clip.mediaPath = link;
    project.timelineClips.push_back(clip);
    check(mvm::project::mediaItemUsage(project).inUse.contains("a"),
          "hard link経由のtimeline参照を使用中と判定しません");
    check(!mvm::project::removeMediaBinEntries(project, {"a"}).success,
          "hard link経由で使用中の素材を削除できてしまいます");

    // canonical Project の比較も実体で行う (recovery の foreign 判定・Save As が使う)。
    check(mvm::project::sameCanonicalPath(original, link),
          "hard linkのProject pathを別物と判定しました");
    check(!mvm::project::sameCanonicalPath(original, other),
          "別ファイルのProject pathを同じと判定しました");

    // 存在しないファイルは表記で比べる。大文字小文字は区別しない。
    Project missing = mvm::project::createDefaultProject();
    missing.mediaItems = {audioItem("m1", "C:/mvm-missing/Tone.wav"),
                          audioItem("m2", R"(c:\MVM-MISSING\tone.WAV)")};
    check(!mvm::project::validateMediaBin(missing).success,
          "大文字小文字と区切り文字だけが違う素材の重複を受理しました");
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
    check(mvm::project::comparePathIdentity(directory, root / "UNAVAILABLE") == PathSameness::Same,
          "表記が同じならidentityを取れなくても同じと判定しません");

    // clip 側は identity 成功、item 側は取得不可。未使用と断定せず、削除を拒否する。
    Project project = mvm::project::createDefaultProject();
    MediaItem item = audioItem("u", "x");
    item.mediaPath = directory;
    project.mediaItems.push_back(item);
    mvm::project::TimelineClip clip;
    clip.mediaPath = file;
    project.timelineClips.push_back(clip);
    const auto usage = mvm::project::mediaItemUsage(project);
    check(!usage.inUse.contains("u") && usage.unknown.contains("u"),
          "identityを取れない素材を未使用と断定しました");
    const Project before = project;
    check(!mvm::project::removeMediaBinEntries(project, {"u"}).success && project == before,
          "使用中か確認できない素材を削除できてしまいます");

    // 対照: clip が無ければ Unknown の相手もいないので削除できる。
    project.timelineClips.clear();
    check(mvm::project::mediaItemUsage(project).unknown.empty() &&
              mvm::project::removeMediaBinEntries(project, {"u"}).success,
          "参照するclipが無い素材を削除できません (対照群)");
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
        R"JSON({"schema_version":6,"format":"mvm-project","timeline_fps_num":60,"timeline_fps_den":1,)JSON"
        R"JSON("video_tracks":[{"name":"V1","muted":false}],"audio_tracks":[],"manim_assets":[],)JSON"
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
    writeText(schema3,
              R"JSON({"schema_version":3,"format":"mvm-project","timeline_fps_num":60,)JSON"
              R"JSON("timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false}],)JSON"
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
    testUnavailableIdentity(root);
    testJson(root);

    if (failures == 0)
        std::printf("media bin: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
