#ifndef MVM_PROJECT_TIMELINE_EDIT_H
#define MVM_PROJECT_TIMELINE_EDIT_H

#include "project/project.h"

#include <string>

namespace mvm::project {

struct TimelineEditResult {
    bool success = false;
    int selectedIndex = -1;
    std::string error;
};

TimelineEditResult moveTimelineClip(Project& project, int selectedIndex, int offset);
TimelineEditResult deleteTimelineClip(Project& project, int selectedIndex);
TimelineEditResult appendManimTimelineClip(Project& project, const ManimAsset& asset);

} // namespace mvm::project

#endif // MVM_PROJECT_TIMELINE_EDIT_H
