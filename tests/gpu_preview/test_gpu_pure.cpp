// mvm Phase 1 / P1 - GPU preview の純粋ロジック単体テスト
//
// GPU も FFmpeg の実 decode も使わない。決定論的に動く部分だけを対象にする。
// 実 decode を伴う検査は test_gpu_decode.cpp にある。
//
// 対象:
//   - PTS <-> frame number 変換 (往復・有理 fps・境界)
//   - color metadata mapping
//   - aspect fit
//   - CPU readback counter (full / marker band を混同しないこと)
//   - frame lifetime (表示後 N 枚は解放されない)
//   - stale generation rejection
//   - device mismatch negative
//   - AV_PIX_FMT_D3D11 以外を選ばないこと (software frame rejection)

#include "media/gpu_preview/color_metadata.h"
#include "media/gpu_preview/ffmpeg_d3d11_decoder.h"
#include "media/gpu_preview/frame_queue.h"
#include "media/gpu_preview/nv12_converter.h"
#include "media/gpu_preview/readback_counter.h"
#include "media/gpu_preview/timebase.h"

#include <cstdio>
#include <string>

using namespace mvm::gpu;

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool cond, const std::string& what) {
    gChecks++;
    if (!cond) {
        gFailures++;
        std::fprintf(stderr, "  FAIL %s\n", what.c_str());
    }
}

void checkEq(long long got, long long want, const std::string& what) {
    gChecks++;
    if (got != want) {
        gFailures++;
        std::fprintf(stderr, "  FAIL %s : got %lld want %lld\n", what.c_str(), got, want);
    }
}

void checkNear(double got, double want, double tol, const std::string& what) {
    gChecks++;
    const double d = got - want;
    if (d > tol || d < -tol) {
        gFailures++;
        std::fprintf(stderr, "  FAIL %s : got %.6f want %.6f\n", what.c_str(), got, want);
    }
}

// --------------------------------------------------------------------------
// PTS <-> frame number
// --------------------------------------------------------------------------
void testTimebase() {
    std::fprintf(stderr, "[timebase]\n");

    // 60fps / time_base 1/15360 (MP4 でよくある値)
    const Rational tb{1, 15360};
    const Rational fps{60, 1};
    checkEq(ptsToFrameNumber(0, 0, tb, fps), 0, "pts 0 -> frame 0");
    checkEq(ptsToFrameNumber(256, 0, tb, fps), 1, "pts 256 -> frame 1");
    checkEq(ptsToFrameNumber(256 * 3599, 0, tb, fps), 3599, "末尾フレーム");

    // start_pts が 0 でない素材
    checkEq(ptsToFrameNumber(1000 + 256 * 137, 1000, tb, fps), 137, "start_pts 補正");

    // 有理 fps (60000/1001)。double を経由すると後半でずれる。
    const Rational tb2{1, 60000};
    const Rational fps2{60000, 1001};
    for (long long f : {0LL, 1LL, 137LL, 299LL, 600LL, 1799LL, 3599LL, 65535LL}) {
        const long long pts = frameNumberToPts(f, 0, tb2, fps2);
        checkEq(ptsToFrameNumber(pts, 0, tb2, fps2), f,
                "往復 60000/1001 frame " + std::to_string(f));
    }

    // microsecond time base。64bit で溢れる中間積を 128bit で持てているか。
    const Rational tbUs{1, 1000000};
    for (long long f : {0LL, 3599LL, 1000000LL}) {
        const long long pts = frameNumberToPts(f, 0, tbUs, fps2);
        checkEq(ptsToFrameNumber(pts, 0, tbUs, fps2), f,
                "往復 microsecond frame " + std::to_string(f));
    }

    // 丸め。半フレーム手前は同じフレームに落ちる。
    checkEq(ptsToFrameNumber(256 * 10 + 127, 0, tb, fps), 10, "半フレーム未満は同じ frame");
    checkEq(ptsToFrameNumber(256 * 10 + 129, 0, tb, fps), 11, "半フレーム超で次の frame");

    // 「分からなかった」を 0 にしない。
    checkEq(ptsToFrameNumber(kNoPts, 0, tb, fps), -1, "pts 不明は -1");
    checkEq(ptsToFrameNumber(0, kNoPts, tb, fps), -1, "start_pts 不明は -1");
    checkEq(ptsToFrameNumber(0, 0, Rational{0, 0}, fps), -1, "time base 不正は -1");
    checkEq(ptsToFrameNumber(0, 0, tb, Rational{0, 1}), -1, "fps 不正は -1");
    checkEq(ptsToFrameNumber(-256, 0, tb, fps), -1, "start_pts より前は -1");
}

// --------------------------------------------------------------------------
// color metadata
// --------------------------------------------------------------------------
void testColor() {
    std::fprintf(stderr, "[color]\n");

    {
        const ColorDecision d = decideColor(avcol::kSpcBt709, avcol::kRangeMpeg, 1920, 1080);
        check(d.space == ColorSpace::BT709, "BT.709 が選ばれる");
        check(d.range == ColorRange::Limited, "limited が選ばれる");
        check(!d.spaceInferred && !d.rangeInferred, "明示指定は推定扱いにしない");
    }
    {
        const ColorDecision d = decideColor(avcol::kSpcBt709, avcol::kRangeJpeg, 1920, 1080);
        check(d.range == ColorRange::Full, "full range を区別する");
    }
    {
        // 未指定は解像度で推定し、推定であることを残す。
        const ColorDecision hd =
            decideColor(avcol::kSpcUnspecified, avcol::kRangeUnspecified, 1920, 1080);
        check(hd.space == ColorSpace::BT709, "HD 未指定は BT.709");
        check(hd.spaceInferred, "推定したことを残す");
        check(hd.range == ColorRange::Limited, "range 未指定は limited");
        check(hd.rangeInferred, "range も推定扱い");

        const ColorDecision sd =
            decideColor(avcol::kSpcUnspecified, avcol::kRangeUnspecified, 720, 480);
        check(sd.space == ColorSpace::BT601, "SD 未指定は BT.601");
    }
    {
        const ColorDecision d = decideColor(avcol::kSpcSmpte170m, avcol::kRangeMpeg, 720, 480);
        check(d.space == ColorSpace::BT601, "SMPTE170M は BT.601");
        check(!d.spaceInferred, "SMPTE170M は推定ではない");
    }
    {
        const ColorDecision d = decideColor(avcol::kSpcBt2020Ncl, avcol::kRangeMpeg, 3840, 2160);
        check(d.space == ColorSpace::BT2020NCL, "BT.2020 NCL");
        const ColorDecision cl = decideColor(avcol::kSpcBt2020Cl, avcol::kRangeMpeg, 3840, 2160);
        check(cl.space == ColorSpace::BT2020NCL, "BT.2020 CL は NCL で代用");
        check(cl.spaceInferred, "代用したことを残す");
    }

    // BT.709 limited の係数。よく知られた値と照合する。
    // 実装と同じ式をテストへ写すと、実装のバグを追認してしまうので定数で書く。
    {
        const YuvToRgbCoefficients k = coefficientsFor(ColorSpace::BT709, ColorRange::Limited);
        checkNear(k.yScale, 1.164383, 1e-5, "709 limited yScale");
        checkNear(k.yOffset, 0.062745, 1e-5, "709 limited yOffset");
        checkNear(k.uvScale, 1.138393, 1e-5, "709 limited uvScale");
        checkNear(k.vr, 1.574800, 1e-5, "709 vr");
        checkNear(k.ub, 1.855600, 1e-5, "709 ub");
        checkNear(k.ug, 0.187324, 1e-5, "709 ug");
        checkNear(k.vg, 0.468124, 1e-5, "709 vg");
    }
    {
        const YuvToRgbCoefficients k = coefficientsFor(ColorSpace::BT709, ColorRange::Full);
        checkNear(k.yScale, 1.0, 1e-9, "709 full yScale");
        checkNear(k.yOffset, 0.0, 1e-9, "709 full yOffset");
        checkNear(k.uvScale, 1.0, 1e-9, "709 full uvScale");
        // full と limited が同じ係数になっていたら、range の区別が効いていない。
        const YuvToRgbCoefficients l = coefficientsFor(ColorSpace::BT709, ColorRange::Limited);
        check(k.yScale != l.yScale, "limited と full が別の係数であること");
    }
    {
        const YuvToRgbCoefficients k = coefficientsFor(ColorSpace::BT601, ColorRange::Limited);
        checkNear(k.vr, 1.402000, 1e-5, "601 vr");
        checkNear(k.ub, 1.772000, 1e-5, "601 ub");
        // 601 と 709 が同じなら、色空間の区別が効いていない。
        const YuvToRgbCoefficients k709 = coefficientsFor(ColorSpace::BT709, ColorRange::Limited);
        check(k.vr != k709.vr, "601 と 709 が別の係数であること");
    }
}

// --------------------------------------------------------------------------
// aspect fit
// --------------------------------------------------------------------------
void testAspectFit() {
    std::fprintf(stderr, "[aspect fit]\n");

    { // 同じ比率なら隙間なく埋まる
        const FitRect r = aspectFit(1920, 1080, 1280, 720);
        checkEq(r.width, 1280, "同比率 width");
        checkEq(r.height, 720, "同比率 height");
        checkEq(r.x, 0, "同比率 x");
        checkEq(r.y, 0, "同比率 y");
    }
    { // 枠の方が縦長 -> 上下に帯
        const FitRect r = aspectFit(1920, 1080, 1000, 1000);
        checkEq(r.width, 1000, "横合わせ width");
        checkEq(r.height, 563, "横合わせ height");
        checkEq(r.y, 218, "上下中央");
    }
    { // 枠の方が横長 -> 左右に帯
        const FitRect r = aspectFit(1080, 1920, 1000, 1000);
        checkEq(r.height, 1000, "縦合わせ height");
        checkEq(r.width, 563, "縦合わせ width");
        checkEq(r.x, 218, "左右中央");
    }
    { // 不正な入力で 0 を返す (負の viewport を作らない)
        const FitRect r = aspectFit(0, 0, 100, 100);
        checkEq(r.width, 0, "不正入力 width 0");
        checkEq(r.height, 0, "不正入力 height 0");
    }
}

// --------------------------------------------------------------------------
// readback counter
// --------------------------------------------------------------------------
void testReadbackCounters() {
    std::fprintf(stderr, "[readback counter]\n");

    ReadbackCounters c;
    checkEq(c.fullFrameReadbacks(), 0, "初期値 0");

    c.noteMarkerBandReadback();
    c.noteMarkerBandReadback();
    c.noteGpuCopy();
    // marker 帯を読んでも full-frame は増えない。
    // ここが混ざると exit criteria (full == 0) が意味を失う。
    checkEq(c.markerBandReadbacks(), 2, "帯 readback を数える");
    checkEq(c.fullFrameReadbacks(), 0, "帯 readback は full を増やさない");
    checkEq(c.gpuCopies(), 1, "GPU copy を数える");

    c.noteFullFrameReadback();
    checkEq(c.fullFrameReadbacks(), 1, "full-frame は別に数える");

    c.reset();
    checkEq(c.fullFrameReadbacks(), 0, "reset");
    checkEq(c.markerBandReadbacks(), 0, "reset (band)");
}

// --------------------------------------------------------------------------
// frame queue
// --------------------------------------------------------------------------
// deviceOfTexture を差し替えて、実 GPU 無しで device mismatch を検査する。
class FakeQueue : public PreviewFrameQueue {
public:
    using PreviewFrameQueue::PreviewFrameQueue;
    ID3D11Device* owner = nullptr;

protected:
    ID3D11Device* deviceOfTexture(ID3D11Texture2D*) const override { return owner; }
};

DecodedGpuFrame makeFrame(long long n, unsigned long long gen, int* freedFlag = nullptr) {
    DecodedGpuFrame f;
    f.frameNumber = n;
    f.pts = n;
    f.timeBase = Rational{1, 60};
    f.width = 1920;
    f.height = 1080;
    f.pixelFormat = GpuPixelFormat::NV12;
    // 実在しないポインタ。参照外ししない経路だけを検査している。
    f.texture = reinterpret_cast<ID3D11Texture2D*>(0x1000 + n);
    f.generation = gen;
    if (freedFlag) {
        *freedFlag = 0;
        f.lifetime = FrameLifetimeToken(freedFlag, [](void* p) { *static_cast<int*>(p) = 1; });
    } else {
        f.lifetime = FrameLifetimeToken(reinterpret_cast<void*>(1), [](void*) {});
    }
    return f;
}

void testFrameQueue() {
    std::fprintf(stderr, "[frame queue]\n");

    { // 正常系と backpressure
        PreviewFrameQueue q(2, 2);
        q.setCurrentGeneration(5);
        check(q.submitFrame(makeFrame(0, 5)) == SubmitResult::Accepted, "受理");
        check(q.submitFrame(makeFrame(1, 5)) == SubmitResult::Accepted, "受理 2");
        // 満杯なら落とさずに拒否する (decode 側が待つ)
        check(q.submitFrame(makeFrame(2, 5)) == SubmitResult::RejectedQueueFull, "満杯は拒否");
        checkEq(static_cast<long long>(q.depth()), 2, "深さ");
        checkEq(q.queueFullCount(), 1, "満杯回数");
    }

    { // stale generation rejection
        PreviewFrameQueue q(4, 2);
        q.setCurrentGeneration(7);
        check(q.submitFrame(makeFrame(100, 6)) == SubmitResult::RejectedStaleGeneration,
              "古い generation を拒否");
        checkEq(q.rejectedStaleCount(), 1, "stale 回数");
        check(q.submitFrame(makeFrame(100, 7)) == SubmitResult::Accepted, "同一 generation は受理");
        check(q.submitFrame(makeFrame(101, 8)) == SubmitResult::Accepted,
              "新しい generation も受理");

        // generation を進めると、表示前の古いフレームは捨てられる。
        q.setCurrentGeneration(9);
        checkEq(static_cast<long long>(q.depth()), 0, "generation 更新で表示待ちを破棄");
    }

    { // device mismatch negative
        auto* expected = reinterpret_cast<ID3D11Device*>(0xAAAA);
        auto* other = reinterpret_cast<ID3D11Device*>(0xBBBB);
        FakeQueue q(4, 2);
        q.setExpectedDevice(expected);

        q.owner = other;
        check(q.submitFrame(makeFrame(1, 0)) == SubmitResult::RejectedDeviceMismatch,
              "別 device の texture を拒否");
        checkEq(q.rejectedDeviceMismatchCount(), 1, "device mismatch 回数");

        q.owner = expected;
        check(q.submitFrame(makeFrame(1, 0)) == SubmitResult::Accepted, "同一 device は受理");
    }

    { // 不正な frame を受理しない
        PreviewFrameQueue q(4, 2);
        DecodedGpuFrame bad = makeFrame(1, 0);
        bad.texture = nullptr;
        check(q.submitFrame(bad) == SubmitResult::RejectedInvalidFrame, "texture 無しは拒否");

        DecodedGpuFrame noToken = makeFrame(1, 0);
        noToken.lifetime.reset();
        check(q.submitFrame(noToken) == SubmitResult::RejectedInvalidFrame,
              "lifetime token 無しは拒否");
    }

    { // frame lifetime: 表示後 retainDepth 枚は解放されない
        PreviewFrameQueue q(8, 2);
        int freed0 = 0, freed1 = 0, freed2 = 0;
        {
            DecodedGpuFrame f0 = makeFrame(0, 0, &freed0);
            DecodedGpuFrame f1 = makeFrame(1, 0, &freed1);
            DecodedGpuFrame f2 = makeFrame(2, 0, &freed2);
            q.submitFrame(f0);
            q.submitFrame(f1);
            q.submitFrame(f2);
        }
        DecodedGpuFrame out;
        check(q.takeForDisplay(out), "取り出し 0");
        q.noteDisplayed(out);
        checkEq(q.displayedFrameNumber(), 0, "描画してから displayed を更新する");
        // まだ queue 内にも参照があるので解放されていない
        checkEq(freed0, 0, "表示直後は解放しない");

        check(q.takeForDisplay(out), "取り出し 1");
        q.noteDisplayed(out);
        check(q.takeForDisplay(out), "取り出し 2");
        q.noteDisplayed(out);
        // retainDepth=2 なので frame 0 は押し出されて解放される
        checkEq(freed0, 1, "retainDepth を超えたら解放される");
        checkEq(freed1, 0, "直近 2 枚は保持される");
        checkEq(freed2, 0, "直近 2 枚は保持される (2)");

        out = DecodedGpuFrame{}; // ローカルの参照を落としてから clear する
        q.clear();
        checkEq(freed1, 1, "clear で解放される");
        checkEq(freed2, 1, "clear で解放される (2)");
        checkEq(q.displayedFrameNumber(), -1, "clear で displayed を戻す");
    }
}

// --------------------------------------------------------------------------
// hardware format の選択 (software frame rejection)
// --------------------------------------------------------------------------
void testHwFormatSelection() {
    std::fprintf(stderr, "[hw format]\n");

    const int d3d11 = expectedHwPixelFormatValue();
    check(isExpectedHwFormat(d3d11), "AV_PIX_FMT_D3D11 は受け入れる");
    check(!isExpectedHwFormat(0), "他の format は受け入れない");
    check(!isExpectedHwFormat(d3d11 + 1), "近い値でも受け入れない");

    { // 候補に D3D11 があれば必ずそれを選ぶ (順序に依存しない)
        const int c[] = {12 /* yuv420p 相当の別値 */, d3d11, -1};
        checkEq(chooseHwPixelFormat(c, 3), d3d11, "候補から D3D11 を選ぶ");
    }
    { // 候補に D3D11 が無ければ software 形式へ落ちない (= AV_PIX_FMT_NONE)
        const int c[] = {0, 1, 2, -1};
        checkEq(chooseHwPixelFormat(c, 4), -1, "software 形式へ落ちない");
    }
    checkEq(chooseHwPixelFormat(nullptr, 0), -1, "null 候補で落ちない");
}

} // namespace

int main() {
    testTimebase();
    testColor();
    testAspectFit();
    testReadbackCounters();
    testFrameQueue();
    testHwFormatSelection();

    std::fprintf(stderr, "\n検査 %d 件 / 失敗 %d 件\n", gChecks, gFailures);
    if (gChecks == 0) {
        std::fprintf(stderr, "検査が 1 件も実行されていません\n");
        return 1;
    }
    return gFailures == 0 ? 0 : 1;
}
