// 実素材で probeMediaFile (素材種別の唯一の判定) を通す。
// 素材は scripts/make-testmedia.ps1 -Mode Smoke が作る (_import/ と既存の Smoke 素材)。
// 期待値は素材の生成条件から手で書く。

#include "media/mlt/mvm_mlt_runtime.h"
#include "media_import.h"
#include "project/media_bin.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;
using mvm::project::MediaKind;

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    ++failures;
}

fs::path utf8Path(const char* text) {
    return fs::path(reinterpret_cast<const char8_t*>(text));
}

std::string describe(const mvm::app::MediaImportResult& result) {
    return result.success ? std::string(mvm::project::mediaKindName(result.item.kind))
                          : "失敗: " + result.error;
}

void expectImage(const fs::path& path, int width, int height) {
    const auto result = mvm::app::probeMediaFile(path);
    check(result.success && result.item.kind == MediaKind::Image && result.item.width == width &&
              result.item.height == height,
          "静止画として読み込めません: " + path.filename().string() + " (" + describe(result) +
              ", " + std::to_string(result.item.width) + "x" + std::to_string(result.item.height) +
              ")");
}

void expectRejected(const fs::path& path, const char* reason) {
    const auto result = mvm::app::probeMediaFile(path);
    check(!result.success && result.error.find(reason) != std::string::npos,
          "「" + std::string(reason) + "」で拒否しません: " + path.filename().string() + " (" +
              describe(result) + ")");
}

void expectAudio(const fs::path& path, int sampleRate) {
    const auto result = mvm::app::probeMediaFile(path);
    // 尺は 1 秒。mp3 / aac は encoder の遅延で前後するので、秒単位の幅で見る。
    const bool durationOk = result.item.durationSamples > sampleRate * 9 / 10 &&
                            result.item.durationSamples < sampleRate * 12 / 10;
    check(result.success && result.item.kind == MediaKind::Audio &&
              result.item.sampleRate == sampleRate && durationOk && result.item.width == 0,
          "音声として読み込めません: " + path.filename().string() + " (" + describe(result) + ", " +
              std::to_string(result.item.sampleRate) + "Hz, " +
              std::to_string(result.item.durationSamples) + " samples)");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_media_import_files <smoke directory>\n");
        return 2;
    }
    const fs::path smoke = utf8Path(argv[1]);
    const fs::path dir = smoke / "_import";
    if (!fs::exists(dir / "jpg_exif_orient6.jpg")) {
        std::fprintf(stderr, "素材がありません。先に pwsh scripts/make-testmedia.ps1 -Mode Smoke を"
                             "実行してください\n");
        return 3;
    }
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLT を初期化できません\n");
        return 4;
    }

    // 静止画。MLT は静止画 GIF / WebP を動画として返すが (実測)、ここでは Image になる。
    for (const char* name : {"png_rgb24.png", "bmp_24.bmp", "tga_24.tga", "qoi_rgb.qoi",
                             "tiff_rgb48.tif", "webp_static.webp", "gif_static.gif"})
        expectImage(dir / name, 64, 48);
    expectImage(dir / "jpg_quadrant.jpg", 64, 32);
    expectImage(dir / "png_pal8_alpha.png", 64, 32);
    expectImage(smoke / "png_alpha.png", 512, 512);
    // 寸法は EXIF の向きを反映した後の値。対照の orientation 1 は元のまま。
    expectImage(dir / "jpg_exif_orient6.jpg", 32, 64);
    expectImage(dir / "jpg_exif_orient1.jpg", 64, 32);
    expectImage(dir / utf8Path("写真　縦向き＆EXIF.jpg"), 32, 64);

    for (const char* name : {"gif_animated.gif", "apng_animated.png", "webp_animated.webp"})
        expectRejected(dir / name, "アニメーション");
    expectRejected(dir / "exr_float.exr", "HDR");
    expectRejected(smoke / "_corrupt" / "text.mp4", "解析できません");

    // カバーアート付きの音声は、MLT が映像ありと返しても Audio になる。
    for (const char* name :
         {"mp3_cover.mp3", "m4a_cover.m4a", "flac_cover.flac", "opus_48k.opus", "vorbis_48k.ogg"})
        expectAudio(dir / name, 48000);

    // 対照: Motion JPEG の動画は codec が JPEG でも Video。
    const auto motionJpeg = mvm::app::probeMediaFile(dir / "mjpeg_av.avi");
    check(motionJpeg.success && motionJpeg.item.kind == MediaKind::Video &&
              motionJpeg.item.frameCount == 10 && motionJpeg.hasAudio,
          "Motion JPEG の動画を Video として読み込めません (" + describe(motionJpeg) + ")");
    // 対照: 本物の映像 + カバーアートの mp4 は Video で、映像は h264 側 (10 frame)。
    const auto coverVideo = mvm::app::probeMediaFile(dir / "mp4_h264_with_cover.mp4");
    check(coverVideo.success && coverVideo.item.kind == MediaKind::Video &&
              coverVideo.item.frameCount == 10 && coverVideo.item.width == 64 &&
              !coverVideo.hasAudio,
          "映像 + カバーアートの mp4 を Video として読み込めません (" + describe(coverVideo) + ")");

    std::printf("media import files: %d 件の検査、失敗 %d 件\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
