#include "timeline_clip_model.h"

#include <cstdio>

#include <QCoreApplication>
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

    if (failures != 0)
        return 1;
    std::puts("M7b-4 timeline model: PASS");
    return 0;
}
