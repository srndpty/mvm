#ifndef MVM_MEDIA_GRAPH_MANIM_BACKEND_H
#define MVM_MEDIA_GRAPH_MANIM_BACKEND_H
#include "media/graph/graph_render.h"

// Graph の Manim backend。Manim 層 (src/media/manim) には依存せず、外部の Python を
// 起動するだけである。Graph の契約 (media/graph) をこの層が実装する。
namespace mvm::graph_manim {
std::variant<graph::GraphBackend, graph::Error> preflightGraph(const std::filesystem::path& python,
                                                               const std::filesystem::path& script,
                                                               const std::filesystem::path& work,
                                                               const std::atomic<bool>* = nullptr);
} // namespace mvm::graph_manim
#endif
