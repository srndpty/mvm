#ifndef MVM_MEDIA_MANIM_GRAPH_H
#define MVM_MEDIA_MANIM_GRAPH_H
#include "media/graph/graph_render.h"

namespace mvm::manim {
struct GraphBackend {
    std::string toolchain;
    graph::GraphRenderer render;
};

std::variant<GraphBackend, graph::Error> preflightGraph(const std::filesystem::path& python,
                                                        const std::filesystem::path& script,
                                                        const std::filesystem::path& work,
                                                        const std::atomic<bool>* = nullptr);
} // namespace mvm::manim
#endif
