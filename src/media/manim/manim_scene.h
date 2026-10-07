#ifndef MVM_MEDIA_MANIM_MANIM_SCENE_H
#define MVM_MEDIA_MANIM_MANIM_SCENE_H

// src/media/manim の内部用。静止・Write・P2 変形 (manim_math_tex.cpp) と Equation Sequence
// (manim_equation_sequence.cpp) が、Manim の起動・失敗の分類・JSON の組み立てを共有する。
// 外へは出さない (math::MathRenderBackend だけが backend の公開の契約)。

#include "media/math/math_render.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace mvm::manim::detail {

// 全 script の共通の先頭 (request.json の読み込みと fail())。
const char* manimScriptPrelude();

struct ManimSceneRun {
    math::MathRenderStatus status = math::MathRenderStatus::Failed;
    std::string message;
    std::string log;
    int width = 0;
    int height = 0;
    std::vector<std::filesystem::path> pngs; // media/images の PNG (名前の順)
};

// script と request.json を作業 directory へ書き、Manim で描く (manim_math_tex.cpp の runScene)。
// 取消・timeout は外部 process の木ごと止める (util/mvm_process の job object)。
ManimSceneRun runManimScene(const std::filesystem::path& manimExecutablePath,
                            const std::filesystem::path& jobDirectory,
                            const std::string& scriptText, const std::wstring& sceneName,
                            const std::string& requestJson, bool lastFrameOnly,
                            std::chrono::milliseconds timeout, const std::atomic<bool>* cancel);

std::string readTextFile(const std::filesystem::path& path);
std::string trimText(std::string line);
void appendJsonText(std::string& json, const std::string& text);
std::string jsonNumber(double value);
// "x" + 16 進 (UTF-8 の byte 列) を戻す。形が違えば false。
bool decodeHexToken(const std::string& token, std::string& text);

} // namespace mvm::manim::detail

#endif // MVM_MEDIA_MANIM_MANIM_SCENE_H
