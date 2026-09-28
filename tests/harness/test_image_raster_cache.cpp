// 画像 clip の preview 用 raster cache。
//   - decode は worker で行い、request は完成まで Loading を返す (GUI thread を止めない)
//   - 同じ素材・同じ出力解像度は同じ instance を返す
//   - 出力解像度を変えたら別の raster を作り、retainOnly で旧解像度を捨てる
//   - 素材を差し替えたら作り直す。size と更新時刻が同じまま中身だけ変わったものは
//     revalidateAll で見つける
//   - byte budget を超えたら、誰も参照していないものから古い順に捨てる
// 素材は scripts/make-testmedia.ps1 -Mode Smoke が _import/ へ作る。

#include "image_raster_cache.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

namespace {

namespace fs = std::filesystem;
using mvm::app::ImageRasterCache;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    ++failures;
}

bool waitUntil(const std::function<bool()>& condition, int timeoutMs = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return true;
}

// Ready になるまで request し直す。
ImageRasterCache::Entry waitReady(ImageRasterCache& cache, const fs::path& path, int width,
                                  int height) {
    ImageRasterCache::Entry entry;
    waitUntil([&] {
        entry = cache.request(path, width, height);
        return entry.state != ImageRasterCache::State::Loading;
    });
    return entry;
}

// 更新時刻は Win32 で直接読み書きする。std::filesystem の実装は秒精度のことがあり、
// 100ns 単位の値を元へ戻せない (test_waveform_cache と同じ)。
bool fileTime(const fs::path& path, FILETIME& time) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = GetFileTime(file, nullptr, nullptr, &time) != 0;
    CloseHandle(file);
    return ok;
}

bool setFileTime(const fs::path& path, const FILETIME& time) {
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = SetFileTime(file, nullptr, nullptr, &time) != 0;
    CloseHandle(file);
    return ok;
}

bool isRed(const mvm::preview::PreviewStillImage& image, int x, int y) {
    const auto offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                         static_cast<std::size_t>(x)) *
                        4U;
    return image.rgba[offset] > 200 && image.rgba[offset + 1] < 60 && image.rgba[offset + 2] < 60;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc != 3) {
        std::fprintf(stderr,
                     "使い方: mvm_test_image_raster_cache <_import directory> <作業 directory>\n");
        return 2;
    }
    const fs::path dir = fs::path(reinterpret_cast<const char8_t*>(argv[1]));
    const fs::path work = fs::path(reinterpret_cast<const char8_t*>(argv[2]));
    if (!fs::exists(dir / "bmp_24.bmp") || !fs::exists(dir / "png_icc_display_p3.png")) {
        std::fprintf(stderr, "素材がありません。先に pwsh scripts/make-testmedia.ps1 -Mode Smoke を"
                             "実行してください\n");
        return 3;
    }
    std::error_code error;
    fs::remove_all(work, error);
    fs::create_directories(work);

    {
        ImageRasterCache cache;
        const fs::path bmp = dir / "bmp_24.bmp";

        // 初回は Loading を返し、worker で作る。
        const auto first = cache.request(bmp, 320, 180);
        check(first.state == ImageRasterCache::State::Loading && !first.image,
              "初回の request が Loading ではありません (GUI thread で decode しています)");
        const auto ready = waitReady(cache, bmp, 320, 180);
        check(ready.state == ImageRasterCache::State::Ready && ready.image &&
                  ready.image->width == 320 && ready.image->height == 180,
              "raster を出力解像度で作れません: " + ready.error.toStdString());
        // 同じ素材・同じ解像度は同じ instance (engine の composition 再送が no-op になる)。
        const auto again = cache.request(bmp, 320, 180);
        check(again.state == ImageRasterCache::State::Ready && again.image == ready.image,
              "同じ素材・解像度で別の instance を返しました");

        // 出力解像度を変えると別の raster。retainOnly で旧解像度を捨てる。
        const auto small = waitReady(cache, bmp, 160, 90);
        check(small.state == ImageRasterCache::State::Ready && small.image &&
                  small.image->width == 160 && small.image != ready.image,
              "出力解像度を変えても別の raster になりません");
        check(cache.entryCount() == 2, "前提: 2 つの解像度の raster がありません");
        cache.retainOnly({ImageRasterCache::keyFor(bmp, 160, 90)});
        check(cache.entryCount() == 1, "旧解像度の raster が retainOnly で捨てられません");
        check(cache.request(bmp, 160, 90).image == small.image,
              "retainOnly で残した raster まで捨てました");

        // 読めない素材は Failed と理由。
        const auto failed = waitReady(cache, dir / "png_icc_display_p3.png", 320, 180);
        check(failed.state == ImageRasterCache::State::Failed && failed.error.contains("ICC"),
              "読めない画像を Failed にしません");
    }

    {
        // 素材の差し替え (size と更新時刻が変わる) は次の request で見つける。
        ImageRasterCache cache;
        const fs::path target = work / "replaced.img";
        fs::copy_file(dir / "bmp_24.bmp", target, fs::copy_options::overwrite_existing);
        const auto before = waitReady(cache, target, 320, 180);
        check(before.state == ImageRasterCache::State::Ready && !isRed(*before.image, 80, 30),
              "前提: 差し替え前の画像の左上が赤です");
        // 左上 1/4 が赤の JPEG に差し替える (拡張子は見ないので同じ path のまま)。
        fs::copy_file(dir / "jpg_quadrant.jpg", target, fs::copy_options::overwrite_existing);
        check(cache.request(target, 320, 180).state == ImageRasterCache::State::Loading,
              "差し替えた素材で古い raster を返しました");
        const auto after = waitReady(cache, target, 320, 180);
        // 64x32 は 320x160 で上下 10 px の余白。左上 1/4 (x < 160, 10 <= y < 90) が赤。
        check(after.state == ImageRasterCache::State::Ready && after.image != before.image &&
                  isRed(*after.image, 80, 40),
              "差し替えた素材の raster になりません");
    }

    {
        // size と更新時刻が同じまま中身だけ変わった素材は、revalidateAll の fingerprint
        // で見つける。
        ImageRasterCache cache;
        const fs::path target = work / "same-size.bmp";
        fs::copy_file(dir / "bmp_24.bmp", target, fs::copy_options::overwrite_existing);
        const auto before = waitReady(cache, target, 320, 180);
        check(before.state == ImageRasterCache::State::Ready, "前提: 画像を読めません");
        FILETIME writeTime{};
        check(fileTime(target, writeTime), "前提: 更新時刻を読めません");
        {
            // BMP は下から上へ行が並ぶ。末尾の数行 (画像の上端) を白にする。
            std::fstream file(target, std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(-64 * 3 * 4, std::ios::end);
            const std::vector<char> white(64 * 3 * 4, static_cast<char>(0xFF));
            file.write(white.data(), static_cast<std::streamsize>(white.size()));
        }
        check(setFileTime(target, writeTime), "前提: 更新時刻を戻せません");
        check(cache.request(target, 320, 180).image == before.image,
              "前提: size と更新時刻が同じなら request では差し替えを見つけません");
        QString changed = QStringLiteral("未通知");
        QObject::connect(&cache, &ImageRasterCache::entryChanged, [&](const QString& key) {
            if (!key.isEmpty())
                changed = key;
        });
        cache.revalidateAll();
        check(waitUntil([&] { return changed == ImageRasterCache::keyFor(target, 320, 180); }),
              "revalidateAll が中身の変わった素材を見つけません");
        const auto after = waitReady(cache, target, 320, 180);
        check(after.state == ImageRasterCache::State::Ready && after.image != before.image,
              "中身の変わった素材の raster を作り直しません");
    }

    {
        // byte budget。参照されている raster は捨てず、参照されていないものから古い順に捨てる。
        ImageRasterCache cache(1, {});
        const fs::path a = dir / "bmp_24.bmp";
        const fs::path b = dir / "png_rgb24.png";
        const fs::path c = dir / "qoi_rgb.qoi";
        auto held = waitReady(cache, a, 64, 36).image;
        waitReady(cache, b, 64, 36);
        check(cache.entryCount() == 2 && cache.request(a, 64, 36).image == held,
              "参照されている raster を budget 超過で捨てました");
        held.reset();
        waitReady(cache, c, 64, 36);
        check(cache.entryCount() == 1 &&
                  cache.request(c, 64, 36).state == ImageRasterCache::State::Ready,
              "参照されなくなった raster を budget 超過で捨てません");
    }

    fs::remove_all(work, error);
    if (failures == 0)
        std::puts("画像 raster cache: 非同期生成・再利用・解像度・差し替え・再検証・budget "
                  "を確認しました");
    return failures == 0 ? 0 : 1;
}
