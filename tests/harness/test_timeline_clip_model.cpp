#include "timeline_clip_model.h"

#include <cstdio>
#include <string>

#include <QCoreApplication>
#include <QPersistentModelIndex>
#include <QSet>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

mvm::project::TimelineClip clip(const char* id, int videoTrackIndex, std::int64_t start,
                                std::int64_t duration) {
    mvm::project::TimelineClip result;
    result.mediaPath = std::string(id) + ".mp4";
    result.name = id;
    result.id = id;
    result.sourceFpsNum = 60;
    result.sourceFpsDen = 1;
    result.sourceFrameCount = duration;
    result.sourceOutFrame = duration;
    result.timelineStartFrame = start;
    result.track = mvm::project::TrackRef{mvm::project::TrackKind::Video, videoTrackIndex};
    return result;
}

// model が出した変更通知を数える。
struct Signals {
    int resets = 0;
    int inserted = 0;
    int removed = 0;
    int changed = 0;
    QList<int> lastRoles;
};

// 受け手 (context) を破棄すると接続が切れる。
void watch(QAbstractItemModel& model, Signals& seen, QObject& context) {
    QObject::connect(&model, &QAbstractItemModel::modelReset, &context, [&seen] { ++seen.resets; });
    QObject::connect(
        &model, &QAbstractItemModel::rowsInserted, &context,
        [&seen](const QModelIndex&, int first, int last) { seen.inserted += last - first + 1; });
    QObject::connect(
        &model, &QAbstractItemModel::rowsRemoved, &context,
        [&seen](const QModelIndex&, int first, int last) { seen.removed += last - first + 1; });
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &context,
                     [&seen](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                         ++seen.changed;
                         seen.lastRoles = roles;
                     });
}

QString idAt(const QAbstractItemModel& model, int row) {
    return model.data(model.index(row, 0), mvm::app::TimelineClipModel::ClipIdRole).toString();
}

// 編集のたびに model を作り直さない。作り直すと timeline の全 delegate を作り直す。
void testIncrementalUpdate() {
    using mvm::app::TimelineClipModel;
    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineClips = {clip("a", 0, 0, 10), clip("b", 0, 10, 10), clip("c", 0, 20, 10)};
    TimelineClipModel model;
    model.setProject(project);
    model.setSelectedClipIds({QStringLiteral("b")});
    const QPersistentModelIndex rowB(model.index(1, 0));
    const QPersistentModelIndex rowC(model.index(2, 0));

    // 値だけの変更 (trim): 行は残し、変わった role だけを知らせる。
    {
        Signals seen;
        QObject context;
        auto trimmed = project;
        trimmed.timelineClips[1].sourceOutFrame = 5;
        trimmed.timelineClips[1].sourceFrameCount = 10;
        watch(model, seen, context);
        model.setProject(trimmed);
        check(seen.resets == 0 && seen.inserted == 0 && seen.removed == 0 && seen.changed == 1,
              "値だけの編集で model を作り直したか、行を消して入れ直しました");
        check(seen.lastRoles.contains(TimelineClipModel::TimelineDurationFramesRole) &&
                  seen.lastRoles.contains(TimelineClipModel::SourceOutFrameRole) &&
                  !seen.lastRoles.contains(TimelineClipModel::DisplayNameRole),
              "値の変わった role だけを知らせません");
        check(rowB.isValid() && rowB.row() == 1 &&
                  model.data(rowB, TimelineClipModel::TimelineDurationFramesRole).toLongLong() == 5,
              "編集した clip の行が保たれないか、値が新しくなりません");
        check(model.data(rowB, TimelineClipModel::SelectedRole).toBool(), "編集で選択が外れました");
        project = trimmed;
    }

    // 変化の無い setProject は何も知らせない。
    {
        Signals seen;
        QObject context;
        watch(model, seen, context);
        model.setProject(project);
        check(seen.resets == 0 && seen.inserted == 0 && seen.removed == 0 && seen.changed == 0,
              "変化の無い setProject で変更を知らせました");
    }

    // 削除: 消えた行だけを消し、後ろの行は残して行番号 (clipRow) だけを知らせる。
    {
        Signals seen;
        QObject context;
        auto deleted = project;
        deleted.timelineClips.erase(deleted.timelineClips.begin());
        watch(model, seen, context);
        model.setProject(deleted);
        check(seen.resets == 0 && seen.removed == 1 && seen.inserted == 0,
              "削除で消えた行以外も消しました");
        check(rowB.isValid() && rowB.row() == 0 && rowC.isValid() && rowC.row() == 1 &&
                  idAt(model, 0) == QStringLiteral("b") && idAt(model, 1) == QStringLiteral("c"),
              "削除の後ろの行が保たれません");
        check(model.data(model.index(1, 0), TimelineClipModel::ClipRowRole).toInt() == 1 &&
                  seen.lastRoles.contains(TimelineClipModel::ClipRowRole),
              "削除で変わった行番号を知らせません");
        project = deleted;
    }

    // 追加 (分割で増えた clip) は入った行だけを入れる。
    {
        Signals seen;
        QObject context;
        auto added = project;
        added.timelineClips.push_back(clip("d", 1, 0, 10));
        watch(model, seen, context);
        model.setProject(added);
        check(seen.resets == 0 && seen.inserted == 1 && seen.removed == 0 &&
                  model.rowCount() == 3 && idAt(model, 2) == QStringLiteral("d"),
              "追加で入った行以外も入れ直しました");
    }
}

// 表示範囲の clip だけを通す。clip 数ではなく表示範囲に比例する数に収まること。
void testClipWindow() {
    using mvm::app::TimelineClipModel;
    using mvm::app::TimelineClipWindowModel;
    constexpr int kClips = 10000;
    mvm::project::Project project = mvm::project::createDefaultProject();
    for (int index = 0; index < kClips; ++index)
        project.timelineClips.push_back(
            clip(("w" + std::to_string(index)).c_str(), 0, index * 10, 10));
    TimelineClipModel model;
    model.setProject(project);
    TimelineClipWindowModel window;
    window.setSourceModel(&model);
    check(window.rowCount() == 0, "表示範囲を受ける前に clip を通しました");

    // 表示範囲 [1000, 1100) (10 clip 分) に左右 1 つ分の余白: [900, 1200) の 30 clip。
    window.setVisibleRange(1000, 1100);
    check(window.rowCount() == 30, "表示範囲と余白に掛かる clip だけを通しません");
    check(idAt(window, 0) == QStringLiteral("w90") && idAt(window, 29) == QStringLiteral("w119"),
          "表示範囲に掛かる clip ではないものを通しました");

    // 余白の内側のスクロールでは絞り直さない。
    {
        Signals seen;
        QObject context;
        watch(window, seen, context);
        window.setVisibleRange(1050, 1150);
        check(seen.inserted == 0 && seen.removed == 0 && seen.resets == 0,
              "余白の内側のスクロールで絞り直しました");
        // 余白を越えたら絞り直す。作り直さず、出入りした行だけを入れ替える。
        window.setVisibleRange(5000, 5100);
        check(window.rowCount() == 30 && idAt(window, 0) == QStringLiteral("w490"),
              "余白を越えたスクロールで絞り直しません");
        check(seen.resets == 0 && seen.inserted == 30 && seen.removed == 30,
              "絞り直しで model を作り直したか、出入りした行以外も入れ替えました");
    }
    // 拡大した後は広すぎる範囲を残さない。
    window.setVisibleRange(5000, 5010);
    check(window.rowCount() == 3, "拡大した後も広い範囲の clip を通し続けます");

    // 範囲外でも固定した (押している) clip は通す。
    window.setPinnedClipIds({QStringLiteral("w0"), QStringLiteral("w9999")});
    check(window.rowCount() == 5 && idAt(window, 0) == QStringLiteral("w0") &&
              idAt(window, 4) == QStringLiteral("w9999"),
          "表示範囲外の固定した clip を通しません");
    window.setPinnedClipIds({});
    check(window.rowCount() == 3, "固定を外した clip が残りました");
    // 選択中の clip は固定しない (全選択で全 clip の delegate を作らない)。
    QSet<QString> all;
    for (int index = 0; index < kClips; ++index)
        all.insert(QStringLiteral("w%1").arg(index));
    model.setSelectedClipIds(all);
    check(window.rowCount() == 3, "全選択で表示範囲外の clip まで通しました");
    model.setSelectedClipIds({});

    // 編集で表示範囲へ入った clip は通し、出た clip は外す (位置の変更で絞り直す)。
    auto moved = project;
    moved.timelineClips[0].timelineStartFrame = 5005;
    moved.timelineClips[500].timelineStartFrame = 100000;
    model.setProject(moved);
    bool hasMovedIn = false;
    bool hasMovedOut = false;
    for (int row = 0; row < window.rowCount(); ++row) {
        hasMovedIn = hasMovedIn || idAt(window, row) == QStringLiteral("w0");
        hasMovedOut = hasMovedOut || idAt(window, row) == QStringLiteral("w500");
    }
    check(hasMovedIn && !hasMovedOut, "編集で表示範囲に出入りした clip を絞り直しません");
}

void testTextClipFilter() {
    using mvm::app::TimelineClipModel;
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto text = clip("text", 1, 0, 10);
    text.kind = mvm::project::TimelineClipKind::Text;
    project.timelineClips = {clip("video", 0, 0, 10), text};
    TimelineClipModel model;
    model.setProject(project);
    mvm::app::TextClipFilterModel texts;
    texts.setSourceModel(&model);
    check(texts.rowCount() == 1 && idAt(texts, 0) == QStringLiteral("text") &&
              texts.data(texts.index(0, 0), TimelineClipModel::ClipRowRole).toInt() == 1,
          "文字 clip だけを全 clip の行番号付きで通しません");

    // 字幕のように短い文字 clip が 10,000 あっても、通すのは再生位置に掛かるものと固定した
    // ものだけ。文字 clip の境界を跨がない再生位置の移動では全行を判定し直さない。
    mvm::project::Project subtitles = mvm::project::createDefaultProject();
    for (int index = 0; index < 10000; ++index) {
        auto line = clip(("line-" + std::to_string(index)).c_str(), 1, index * 10LL, 10);
        line.kind = mvm::project::TimelineClipKind::Text;
        subtitles.timelineClips.push_back(std::move(line));
    }
    TimelineClipModel many;
    many.setProject(subtitles);
    mvm::app::TextClipFilterModel active;
    active.setSourceModel(&many);
    active.setPlayheadFrame(5005);
    check(active.rowCount() == 1 && idAt(active, 0) == QStringLiteral("line-500"),
          "再生位置に掛かる文字 clip だけを通しません");
    const auto refilters = active.playheadRefilterCountForTest();
    active.setPlayheadFrame(5009);
    check(active.playheadRefilterCountForTest() == refilters && active.rowCount() == 1,
          "文字 clip の境界を跨がない再生位置の移動で全行を判定し直しました");
    active.setPlayheadFrame(5010);
    check(active.rowCount() == 1 && idAt(active, 0) == QStringLiteral("line-501") &&
              active.playheadRefilterCountForTest() == refilters + 1,
          "文字 clip の境界を跨いだ再生位置で絞り直しません");
    // 前へ戻っても絞り直す (範囲の下限)。
    active.setPlayheadFrame(5009);
    check(active.rowCount() == 1 && idAt(active, 0) == QStringLiteral("line-500"),
          "再生位置を戻したときに絞り直しません");
    // ドラッグしている文字 clip は、再生位置から外れても残す。
    active.setPinnedClipIds({QStringLiteral("line-0")});
    check(active.rowCount() == 2, "固定した文字 clip を通しません");
    active.setPinnedClipIds({});
    check(active.rowCount() == 1, "固定を外した文字 clip が残っています");
    // 編集で再生位置に掛かるようになった文字 clip は、範囲の内側でも通す。
    auto moved = subtitles;
    moved.timelineClips[0].timelineStartFrame = 5000;
    moved.timelineClips[0].sourceOutFrame = 10;
    many.setProject(moved);
    check(active.rowCount() == 2, "編集で再生位置に掛かった文字 clip を通しません");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    mvm::project::Project project = mvm::project::createDefaultProject();
    // vector順を時間順・track順のどちらにもせず、model roleをauthorityとして検査する。
    project.timelineClips = {clip("late-v1", 0, 500, 40), clip("early-v2", 1, 25, 80),
                             clip("early-v1", 0, 100, 60)};
    project.timelineClips[1].linkGroupId = "link";

    mvm::app::TimelineClipModel model;
    model.setProject(project);
    check(model.rowCount() == 3, "全clipがtimeline modelへ公開されません");
    const double pixelsPerFrame = 1.5;

    const struct Expected {
        std::int64_t start;
        std::int64_t duration;
        int track;
        double x;
        double width;
    } expected[] = {{500, 40, 0, 750.0, 60.0}, {25, 80, 1, 37.5, 120.0}, {100, 60, 0, 150.0, 90.0}};

    for (int row = 0; row < model.rowCount(); ++row) {
        const QModelIndex index = model.index(row, 0);
        const auto start =
            model.data(index, mvm::app::TimelineClipModel::TimelineStartFrameRole).toLongLong();
        const auto duration =
            model.data(index, mvm::app::TimelineClipModel::TimelineDurationFramesRole).toLongLong();
        const auto track = model.data(index, mvm::app::TimelineClipModel::TrackIndexRole).toInt();
        check(start == expected[row].start && duration == expected[row].duration &&
                  track == expected[row].track,
              "vector順に依存せずtrack/start/durationを公開できません");
        check(static_cast<double>(start) * pixelsPerFrame == expected[row].x &&
                  static_cast<double>(duration) * pixelsPerFrame == expected[row].width,
              "timeline geometryのframe換算が一致しません");
        const bool linked = model.data(index, mvm::app::TimelineClipModel::LinkedRole).toBool();
        check(linked == (row == 1), "clipのリンク状態をmodel roleへ公開できません");
        const QString linkGroup =
            model.data(index, mvm::app::TimelineClipModel::LinkGroupIdRole).toString();
        check(linkGroup == (row == 1 ? QStringLiteral("link") : QString()),
              "drag preview用link groupをmodel roleへ公開できません");
        // 波形は素材 path ごとに生成する。clip id ではなく素材 path を出すこと。
        const QString id = model.data(index, mvm::app::TimelineClipModel::ClipIdRole).toString();
        const QString mediaPath =
            model.data(index, mvm::app::TimelineClipModel::MediaPathRole).toString();
        check(mediaPath == id + QStringLiteral(".mp4"),
              "波形用の素材pathをmodel roleへ公開できません");
    }
    check(model.roleNames().value(mvm::app::TimelineClipModel::MediaPathRole) == "mediaPath",
          "QMLから素材pathを参照するrole名がありません");

    model.setSelectedClipIds(QSet<QString>{QStringLiteral("late-v1"), QStringLiteral("early-v1")});
    check(model.data(model.index(0, 0), mvm::app::TimelineClipModel::SelectedRole).toBool() &&
              !model.data(model.index(1, 0), mvm::app::TimelineClipModel::SelectedRole).toBool() &&
              model.data(model.index(2, 0), mvm::app::TimelineClipModel::SelectedRole).toBool(),
          "矩形選択した複数clipをmodel roleへ公開できません");

    testIncrementalUpdate();
    testClipWindow();
    testTextClipFilter();

    if (failures != 0)
        return 1;
    std::puts("M7b-4 timeline model: PASS");
    return 0;
}
