#ifndef MVM_APP_GRAPH_RENDER_COMPILE_H
#define MVM_APP_GRAPH_RENDER_COMPILE_H
#include "media/graph/graph_render.h"
#include "project/graph_clip.h"

namespace mvm::app {
graph::SpecResult compileGraphRender(const project::GraphClipData&, std::int64_t sourceFrames,
                                     int width, int height);
}
#endif
