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
#include "media/gpu_preview/composed_frame.h"
#include "media/gpu_preview/composition_display_ledger.h"
#include "media/gpu_preview/compositor_coordinator.h"
#include "media/gpu_preview/device_change.h"
#include "media/gpu_preview/display_ledger.h"
#include "media/gpu_preview/exact_frame_pairer.h"
#include "media/gpu_preview/ffmpeg_d3d11_decoder.h"
#include "media/gpu_preview/frame_queue.h"
#include "media/gpu_preview/gpu_completion.h"
#include "media/gpu_preview/lifecycle.h"
#include "media/gpu_preview/nv12_converter.h"
#include "media/gpu_preview/output_scheduler.h"
#include "media/gpu_preview/readback_counter.h"
#include "media/gpu_preview/source_frame_buffer.h"
#include "media/gpu_preview/source_registry.h"
#include "media/gpu_preview/timebase.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

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
    f.sourceId = SourceId{1};
    f.sourceGeneration = SourceGeneration{gen};
    f.resourceEpoch = ResourceEpoch{1};
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
        PreviewFrameQueue q(2);
        check(q.registerSource(SourceId{1}, SourceGeneration{5}), "source を登録");
        check(q.submitFrame(makeFrame(0, 5)) == SubmitResult::Accepted, "受理");
        check(q.submitFrame(makeFrame(1, 5)) == SubmitResult::Accepted, "受理 2");
        // 満杯なら落とさずに拒否する (decode 側が待つ)
        check(q.submitFrame(makeFrame(2, 5)) == SubmitResult::RejectedQueueFull, "満杯は拒否");
        checkEq(static_cast<long long>(q.depth()), 2, "深さ");
        checkEq(q.queueFullCount(), 1, "満杯回数");
    }

    { // device mismatch negative
        auto* expected = reinterpret_cast<ID3D11Device*>(0xAAAA);
        auto* other = reinterpret_cast<ID3D11Device*>(0xBBBB);
        FakeQueue q(4);
        // **source を先に登録する。** 未登録の source は fail-closed で
        // 拒否されるようになった (P1.2 §2)。
        check(q.registerSource(SourceId{1}, SourceGeneration{0}), "source を登録");
        q.setExpectedDevice(expected);

        q.owner = other;
        check(q.submitFrame(makeFrame(1, 0)) == SubmitResult::RejectedDeviceMismatch,
              "別 device の texture を拒否");
        checkEq(q.rejectedDeviceMismatchCount(), 1, "device mismatch 回数");

        q.owner = expected;
        check(q.submitFrame(makeFrame(1, 0)) == SubmitResult::Accepted, "同一 device は受理");
    }

    { // 不正な frame を受理しない
        PreviewFrameQueue q(4);
        check(q.registerSource(SourceId{1}, SourceGeneration{0}), "source を登録");
        DecodedGpuFrame bad = makeFrame(1, 0);
        bad.texture = nullptr;
        check(q.submitFrame(bad) == SubmitResult::RejectedInvalidFrame, "texture 無しは拒否");
        checkEq(q.rejectedInvalidCount(), 1, "invalid を数える");

        DecodedGpuFrame noToken = makeFrame(1, 0);
        noToken.lifetime.reset();
        check(q.submitFrame(noToken) == SubmitResult::RejectedInvalidFrame,
              "lifetime token 無しは拒否");
    }

    { // noteDisplayed は番号だけを更新する (retain は GpuRetirementQueue の責務)
        PreviewFrameQueue q(8);
        check(q.registerSource(SourceId{1}, SourceGeneration{0}), "source を登録");
        int freed0 = 0;
        {
            DecodedGpuFrame f0 = makeFrame(0, 0, &freed0);
            q.submitFrame(f0);
        }
        DecodedGpuFrame out;
        check(q.takeForDisplay(out), "取り出し 0");
        q.noteDisplayed(out);
        checkEq(q.displayedFrameNumber(), 0, "描画してから displayed を更新する");
        // queue は retain しない。ローカル out が唯一の参照。
        checkEq(freed0, 0, "out が生きている間は解放されない");
        out = DecodedGpuFrame{};
        checkEq(freed0, 1, "最後の参照を落とすと解放される (queue は retain しない)");
        q.clear();
        checkEq(q.displayedFrameNumber(), -1, "clear で displayed を戻す");
    }
}

// --------------------------------------------------------------------------
// generation 契約 (§3): future 拒否・逆行拒否・同値 no-op
// --------------------------------------------------------------------------
void testGenerationContract() {
    std::fprintf(stderr, "[generation contract]\n");

    { // stale / accepted / future
        PreviewFrameQueue q(8);
        check(q.registerSource(SourceId{1}, SourceGeneration{7}), "source を登録");
        check(q.submitFrame(makeFrame(100, 6)) == SubmitResult::RejectedStaleGeneration,
              "過去 generation は stale 拒否");
        checkEq(q.rejectedStaleCount(), 1, "stale 回数");
        check(q.submitFrame(makeFrame(100, 7)) == SubmitResult::Accepted, "同一 generation は受理");
        // 未来の generation は表示側がまだ知らない世代。fail-closed で拒否する。
        check(q.submitFrame(makeFrame(101, 8)) == SubmitResult::RejectedFutureGeneration,
              "未来 generation は future 拒否");
        checkEq(q.rejectedFutureCount(), 1, "future 回数");
    }

    { // setCurrentGeneration の 3 分岐
        PreviewFrameQueue q(8);
        check(q.registerSource(SourceId{1}, SourceGeneration{5}), "初回は登録");
        // 前進: pending を破棄する
        check(q.submitFrame(makeFrame(0, 5)) == SubmitResult::Accepted, "受理");
        checkEq(static_cast<long long>(q.setCurrentGeneration(SourceId{1}, SourceGeneration{6})),
                static_cast<long long>(GenerationUpdateResult::Updated), "前進は更新");
        checkEq(static_cast<long long>(q.depth()), 0, "前進で pending 破棄");

        // 同値: no-op。**pending は破棄しない。**
        check(q.submitFrame(makeFrame(1, 6)) == SubmitResult::Accepted, "受理 2");
        checkEq(static_cast<long long>(q.setCurrentGeneration(SourceId{1}, SourceGeneration{6})),
                static_cast<long long>(GenerationUpdateResult::NoOp), "同値は no-op");
        checkEq(static_cast<long long>(q.depth()), 1, "同値設定で pending は消えない");

        // 逆行: 拒否。current は変わらない。
        checkEq(static_cast<long long>(q.setCurrentGeneration(SourceId{1}, SourceGeneration{5})),
                static_cast<long long>(GenerationUpdateResult::RejectedRegression), "逆行は拒否");
        checkEq(q.generationRegressionCount(), 1, "逆行回数");
        check(q.currentGeneration(SourceId{1}) == SourceGeneration{6},
              "逆行で current は変わらない");
        checkEq(static_cast<long long>(q.depth()), 1, "逆行で pending も変わらない");
    }

    { // **source ごとに独立していること (P1.2 §2)**
        // source A の seek が source B のフレームを stale / future にしない。
        PreviewFrameQueue q(8);
        check(q.registerSource(SourceId{1}, SourceGeneration{5}), "source A を登録");
        check(q.registerSource(SourceId{2}, SourceGeneration{1}), "source B を登録");

        DecodedGpuFrame a = makeFrame(10, 5);
        a.sourceId = SourceId{1};
        a.sourceGeneration = SourceGeneration{5};
        a.resourceEpoch = ResourceEpoch{7};

        DecodedGpuFrame b = makeFrame(20, 1);
        b.sourceId = SourceId{2};
        b.sourceGeneration = SourceGeneration{1};
        b.resourceEpoch = ResourceEpoch{8};

        // 別 decoder は別の resource epoch を持つ
        check(a.resourceEpoch != b.resourceEpoch, "decoder A と B は別の resourceEpoch");

        check(q.submitFrame(a) == SubmitResult::Accepted, "source A を受理");
        check(q.submitFrame(b) == SubmitResult::Accepted, "source B も受理");

        // source A だけ seek する
        checkEq(static_cast<long long>(q.setCurrentGeneration(SourceId{1}, SourceGeneration{6})),
                static_cast<long long>(GenerationUpdateResult::Updated),
                "A の generation を進める");
        // **B の generation は変わらない**
        check(q.currentGeneration(SourceId{2}) == SourceGeneration{1},
              "A の seek が B の generation を変えない");

        // B の同世代フレームは今も受理される
        DecodedGpuFrame b2 = makeFrame(21, 1);
        b2.sourceId = SourceId{2};
        b2.sourceGeneration = SourceGeneration{1};
        check(q.submitFrame(b2) == SubmitResult::Accepted, "B は A の seek の影響を受けない");

        // A の旧世代は stale
        check(q.submitFrame(a) == SubmitResult::RejectedStaleGeneration, "A の旧世代は stale");
    }

    { // 知らない source は受理しない (fail-closed)
        PreviewFrameQueue q(4);
        check(q.registerSource(SourceId{1}, SourceGeneration{1}), "既知 source を登録");
        DecodedGpuFrame other = makeFrame(0, 1);
        other.sourceId = SourceId{99};
        check(q.submitFrame(other) == SubmitResult::RejectedFutureGeneration,
              "知らない source は受理しない");
    }

    { // composition epoch は compositor が持つ。decoder は発行しない
        PreviewFrameQueue q(4);
        check(q.compositionEpoch() == CompositionEpoch{}, "初期値は 0");
        q.setCompositionEpoch(CompositionEpoch{3});
        check(q.compositionEpoch() == CompositionEpoch{3}, "compositor が設定する");
    }
}

// --------------------------------------------------------------------------
// display ledger (P1.2 §1)
// --------------------------------------------------------------------------
void testDisplayLedger() {
    std::fprintf(stderr, "[display ledger]\n");

    const SourceId src{1};
    const SourceGeneration gen{5};
    const CompositionEpoch epoch{2};

    DisplayWaitKey key;
    key.sourceId = src;
    key.sourceGeneration = gen;
    key.compositionEpoch = epoch;
    key.requestedFrame = 137;

    auto frameFor = [&](long long n, SourceGeneration g) {
        DecodedGpuFrame f = makeFrame(n, g.value);
        f.sourceId = src;
        f.sourceGeneration = g;
        return f;
    };

    { // **これが P1.1 の race そのものである。**
      // baseline を取った後、待機を始める *前* に display が起きる。
      // 旧方式 (arm してから次の display を待つ) なら永久に来ないので timeout した。
      // 新方式は記録済みのものを見つけるので、待たずに成功する。
        DisplayLedger ledger;
        const unsigned long long baseline = ledger.currentSequence();

        // waiter が arm する前に render thread が描いた
        ledger.recordDisplay(frameFor(137, gen), epoch, 12345);

        DisplayRecord rec;
        // timeout 0 で呼ぶ。**sleep も待機もせずに見つからなければ失敗**なので、
        // 「記録済みを拾えること」だけを決定論的に検査できる。
        check(ledger.waitForDisplay(baseline, key, 0, rec),
              "arm 前に描かれた display を取りこぼさない");
        checkEq(rec.displayedFrame, 137, "取得した frame 番号");
        checkEq(static_cast<long long>(rec.displayedQpc), 12345, "取得した qpc");
        check(rec.sequence > baseline, "baseline より後の記録である");
    }

    { // **古い display を新しい request の成功にしない。**
      // baseline より前の記録は、条件が全部一致していても採用しない。
        DisplayLedger ledger;
        ledger.recordDisplay(frameFor(137, gen), epoch, 111);         // 先に描かれた
        const unsigned long long baseline = ledger.currentSequence(); // その後で baseline

        DisplayRecord rec;
        check(!ledger.waitForDisplay(baseline, key, 0, rec),
              "baseline 以前の display は採用しない");
    }

    { // generation / source / epoch / frame のどれか 1 つでも違えば採用しない
        DisplayLedger ledger;
        const unsigned long long baseline = ledger.currentSequence();
        DisplayRecord rec;

        ledger.recordDisplay(frameFor(137, SourceGeneration{4}), epoch, 1);
        check(!ledger.waitForDisplay(baseline, key, 0, rec), "generation 違いは採用しない");

        DecodedGpuFrame otherSource = frameFor(137, gen);
        otherSource.sourceId = SourceId{2};
        ledger.recordDisplay(otherSource, epoch, 2);
        check(!ledger.waitForDisplay(baseline, key, 0, rec), "source 違いは採用しない");

        ledger.recordDisplay(frameFor(137, gen), CompositionEpoch{9}, 3);
        check(!ledger.waitForDisplay(baseline, key, 0, rec), "composition epoch 違いは採用しない");

        ledger.recordDisplay(frameFor(138, gen), epoch, 4);
        check(!ledger.waitForDisplay(baseline, key, 0, rec), "frame 違いは採用しない");

        // 最後に正しいものを入れると成功する (対照)
        ledger.recordDisplay(frameFor(137, gen), epoch, 5);
        check(ledger.waitForDisplay(baseline, key, 0, rec), "全一致なら採用する");
        checkEq(static_cast<long long>(rec.displayedQpc), 5, "最後に入れた記録を取る");
    }

    { // 別スレッドから記録されたものを待てる (通知経路)。
      // 記録側が先に走っても後に走っても成立する。sleep には依存しない。
        DisplayLedger ledger;
        const unsigned long long baseline = ledger.currentSequence();
        std::thread writer([&] { ledger.recordDisplay(frameFor(137, gen), epoch, 777); });
        DisplayRecord rec;
        const bool got = ledger.waitForDisplay(baseline, key, 5000, rec);
        writer.join();
        check(got, "別スレッドの記録を待って取得できる");
        checkEq(static_cast<long long>(rec.displayedQpc), 777, "待って取得した qpc");
    }

    { // abort すると待たずに false
        DisplayLedger ledger;
        ledger.abort();
        DisplayRecord rec;
        check(!ledger.waitForDisplay(0, key, 0, rec), "abort 後は待たない");
    }
}

// --------------------------------------------------------------------------
// device change の停止順序 (P1.2 §3)
// --------------------------------------------------------------------------
void testDeviceChangeOrder() {
    std::fprintf(stderr, "[device change order]\n");

    DeviceChangeCoordinator c;
    check(c.state() == DeviceChangeState::None, "初期状態");
    check(!c.mayTeardown(), "検出前に teardown してはいけない");
    checkEq(c.detectedCount(), 0, "検出 0 件");

    c.noteDetected("device が差し替わった");
    check(c.state() == DeviceChangeState::Detected, "検出済み");
    checkEq(c.detectedCount(), 1, "検出 1 件");
    check(c.detected(), "GUI から検出が見える");

    // **decode thread の join が済むまで teardown を許さない。**
    // ここを緩めると、decode 中の device を Release することになる。
    check(!c.mayTeardown(), "worker 停止前は teardown 不可");

    c.noteWorkerStopped();
    check(c.state() == DeviceChangeState::WorkerStopped, "worker 停止済み");
    check(!c.detected(), "停止後は detected を再処理しない");
    check(c.mayTeardown(), "worker 停止後は teardown 可");

    c.noteTornDown();
    check(c.state() == DeviceChangeState::TornDown, "teardown 済み");
    check(!c.mayTeardown(), "teardown 後は二重に行わない");

    // 二重検出で段階を巻き戻さない
    c.noteDetected("二度目");
    check(c.state() == DeviceChangeState::TornDown, "段階を巻き戻さない");
    checkEq(c.detectedCount(), 1, "二重検出を数え直さない");

    // **P1.2 は復帰を実装していない。** handled は 0 のまま。
    c.noteFailClosed();
    checkEq(c.failClosedCount(), 1, "fail-closed を数える");
    checkEq(c.handledCount(), 0, "復帰は実装していないので handled は 0");

    // device change も共通 lifecycle の終端まで同じ順序を通る。
    LifecycleCoordinator lifecycle;
    check(lifecycle.requestDecodeStop("device change", true), "検出後に stop を要求");
    check(lifecycle.noteDecodeStopped(), "device change の worker join");
    check(lifecycle.requestRenderTeardown(), "join 後に render teardown を要求");
    ShutdownReport report;
    report.teardownSuccess = true;
    check(lifecycle.noteRenderTornDown(report), "device change の render teardown 完了");
    check(lifecycle.noteFinalReportWritten(), "device change でも teardown 後に JSON");
}

// --------------------------------------------------------------------------
// 共通 shutdown state machine (P1.2-finalize)
// --------------------------------------------------------------------------
void testLifecycleOrder() {
    std::fprintf(stderr, "[shutdown lifecycle]\n");

    LifecycleCoordinator c;
    check(c.state() == ShutdownState::Running, "shutdown 初期状態");
    check(c.requestDecodeStop("通常完了", false), "decode stop を要求");
    check(c.noteDecodeStopped(), "worker join を記録");
    check(c.requestRenderTeardown(), "render teardown を要求");
    check(c.mayTeardown(), "join 後だけ teardown 可能");

    ShutdownReport written;
    written.teardownSuccess = true;
    written.retirementDepthAfterDrain = 0;
    check(c.noteRenderTornDown(written), "render teardown 完了を記録");
    ShutdownReport observed;
    check(c.waitForRenderTornDown(0, observed), "condition_variable から完了を取得");
    check(observed.teardownSuccess, "teardown report を保持");
    check(c.noteFinalReportWritten(), "teardown 後に final report を記録");
    check(c.noteExit(), "final report 後に exit");
    check(c.state() == ShutdownState::Exit, "正常な最終状態");
    checkEq(c.orderViolationCount(), 0, "正常順序は違反 0");

    LifecycleCoordinator negative;
    check(!negative.requestRenderTeardown(), "worker running 中の teardown を拒否");
    check(!negative.mayTeardown(), "順序違反後も teardown 不可");
    negative.noteDestructorFallback();
    checkEq(negative.orderViolationCount(), 2, "destructor fallback も違反として記録");
    check(negative.fatal(), "destructor fallback は fatal shutdown を要求");
    check(!negative.report().teardownSuccess, "fallback を成功扱いしない");

    // worker running の fallback では shared resource の release callback を
    // 一度も呼ばない、という renderer の分岐条件を決定論的に固定する。
    int retirementReleaseCount = 0;
    int converterReleaseCount = 0;
    int completionReleaseCount = 0;
    int deviceReleaseCount = 0;
    const bool mayReleaseShared = negative.mayTeardown();
    if (mayReleaseShared) {
        ++retirementReleaseCount;
        ++converterReleaseCount;
        ++completionReleaseCount;
        ++deviceReleaseCount;
    }
    checkEq(retirementReleaseCount, 0, "fallback は retirement を解放しない");
    checkEq(converterReleaseCount, 0, "fallback は converter を解放しない");
    checkEq(completionReleaseCount, 0, "fallback は completion を解放しない");
    checkEq(deviceReleaseCount, 0, "fallback は shared device を解放しない");

    LifecycleCoordinator normal;
    check(normal.requestDecodeStop("通常終了", false), "通常終了の stop 要求");
    check(normal.noteDecodeStopped(), "通常終了の join");
    check(normal.requestRenderTeardown(), "通常 teardown の要求");
    int normalReleaseCount = 0;
    if (normal.mayTeardown())
        ++normalReleaseCount;
    ShutdownReport normalReport;
    normalReport.teardownSuccess = true;
    check(normal.noteRenderTornDown(normalReport), "通常 teardown を記録");
    if (normal.mayTeardown())
        ++normalReleaseCount;
    checkEq(normalReleaseCount, 1, "通常 teardown は一度だけ実行する");
}

void testCompletionFatalStopsRendering() {
    std::fprintf(stderr, "[completion fatal gate]\n");
    RenderFatalGate gate;
    check(gate.tryBeginDraw(), "fatal 前は draw 可能");
    gate.noteSubmission();
    check(gate.noteFatal(), "最初の fatal を記録");
    const long long draws = gate.drawCount();
    const long long submissions = gate.submissionCount();
    check(!gate.tryBeginDraw(), "fatal 後の draw を拒否");
    checkEq(gate.drawCount(), draws, "fatal 後に draw 数が増えない");
    checkEq(gate.submissionCount(), submissions, "fatal 後に submission 数が増えない");
    check(!gate.noteFatal(), "二度目の fatal は最初として扱わない");
}

void testSourceLifecycle() {
    std::fprintf(stderr, "[source lifecycle]\n");
    PreviewFrameQueue q(8);
    const SourceId a{1};
    const SourceId b{2};
    check(q.registerSource(a, SourceGeneration{5}), "A を登録");
    check(q.registerSource(b, SourceGeneration{9}), "B を登録");
    check(q.unregisterSource(a), "A を解除");
    check(q.submitFrame(makeFrame(0, 5)) == SubmitResult::RejectedFutureGeneration,
          "解除後の A frame を拒否");
    DecodedGpuFrame bf = makeFrame(1, 9);
    bf.sourceId = b;
    check(q.submitFrame(bf) == SubmitResult::Accepted, "A 解除後も B は継続");
    check(q.currentGeneration(b) == SourceGeneration{9}, "A 再 open は B を変えない");

    for (unsigned long long i = 0; i < 1000; ++i) {
        const SourceId id{100 + i};
        check(q.registerSource(id, SourceGeneration{1}), "soak source を登録");
        check(q.unregisterSource(id), "soak source を解除");
    }
    checkEq(static_cast<long long>(q.registeredSourceCount()), 1,
            "1000 回 open/close 後も登録数は増えない");
}

void testCompositionAdoptionEpoch() {
    std::fprintf(stderr, "[composition adoption epoch]\n");
    const DecodedGpuFrame decoded = makeFrame(10, 1);
    CompositionEpoch mutableEpoch{3};
    const ComposedFrame adopted = adoptForComposition(decoded, mutableEpoch);
    mutableEpoch = CompositionEpoch{4};
    check(adopted.compositionEpoch == CompositionEpoch{3},
          "composition 採用時点の epoch を値で固定する");
}

void testP2SourceAndComposition() {
    std::fprintf(stderr, "[P2 source / composition]\n");
    SourceRegistry registry;
    const SourceId a = registry.registerSource();
    const SourceId b = registry.registerSource();
    check(a != b, "SourceId は一意");
    checkEq(static_cast<long long>(registry.registeredSourceCount()), 2, "source 登録数 2");
    check(registry.unregisterSource(a), "A を unregister");
    check(registry.contains(b), "A unregister 後も B を保持");
    check(!registry.contains(SourceId{999}), "unknown SourceId を認識しない");

    SourceFrameBuffer bufferA(a, SourceGeneration{1}, 2);
    SourceFrameBuffer bufferB(b, SourceGeneration{7}, 2);
    auto staleA = makeFrame(0, 0);
    staleA.sourceId = a;
    check(bufferA.submitFrame(staleA) == SubmitResult::RejectedStaleGeneration,
          "source-local buffer は stale generation を区別して拒否");
    auto futureA = makeFrame(0, 2);
    futureA.sourceId = a;
    check(bufferA.submitFrame(futureA) == SubmitResult::RejectedFutureGeneration,
          "source-local buffer は future generation を区別して拒否");
    check(bufferA.setGeneration(SourceGeneration{2}) == GenerationUpdateResult::Updated,
          "A seek で A generation を進める");
    check(bufferB.generation() == SourceGeneration{7}, "A seek は B generation を変えない");
    bufferA.stop();
    check(bufferA.stopped(), "A buffer を stop");
    check(!bufferB.stopped(), "A stop は B buffer を止めない");

    CompositorCoordinator coordinator;
    const std::vector<LayerLayout> layout = {
        {a, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0},
        {b, {0.5f, 0.5f, 0.5f, 0.5f}, {0, 0, 1, 1}, 0.75f, 1},
    };
    check(coordinator.configure(layout, {{a, {2}}, {b, {7}}}) == ConfigureResult::Configured,
          "2 source layout を構成");
    check(coordinator.compositionEpoch() == CompositionEpoch{1}, "configure 後の epoch は 1");
    check(coordinator.configure(layout, {{a, {2}}, {b, {7}}}) ==
              ConfigureResult::RejectedAlreadyConfigured,
          "再 configure を拒否");
    check(coordinator.compositionEpoch() == CompositionEpoch{1},
          "再 configure で epoch を巻き戻さない");
    check(coordinator.updateLayout(layout) == LayoutUpdateResult::NoOp, "同一 layout は no-op");
    check(coordinator.compositionEpoch() == CompositionEpoch{1}, "同一 layout で epoch を進めない");

    auto fa = makeFrame(10, 2);
    fa.sourceId = a;
    auto fb = makeFrame(10, 7);
    fb.sourceId = b;
    ComposedFrame composed;
    check(coordinator.compose(10, {fa, fb}, composed) == CompositionResult::Accepted,
          "A=N/B=N だけを合成");
    checkEq(static_cast<long long>(composed.layers.size()), 2, "layer は 2 枚");
    check(composed.layers[0].frame.sourceId == a && composed.layers[1].frame.sourceId == b,
          "z-order は決定論的");

    auto oldB = fb;
    oldB.frameNumber = 9;
    check(coordinator.compose(10, {fa, oldB}, composed) == CompositionResult::MixedFrame,
          "A=N/B=N-1 を拒否");
    checkEq(coordinator.mixedSourceFrameCount(), 1, "mixed frame を数える");

    auto stale = fb;
    stale.sourceGeneration = SourceGeneration{6};
    check(coordinator.compose(10, {fa, stale}, composed) == CompositionResult::StaleGeneration,
          "stale source generation を拒否");
    auto future = fb;
    future.sourceGeneration = SourceGeneration{8};
    check(coordinator.compose(10, {fa, future}, composed) == CompositionResult::FutureGeneration,
          "future source generation を拒否");

    check(coordinator.compose(10, {fa}, composed) == CompositionResult::MissingSource,
          "source missing を拒否");
    auto unknown = fb;
    unknown.sourceId = SourceId{999};
    check(coordinator.compose(10, {fa, unknown}, composed) == CompositionResult::UnknownSource,
          "unknown SourceId を fail-closed で拒否");

    check(coordinator.compose(10, {fa, fb}, composed) == CompositionResult::Accepted,
          "epoch test 用に採用");
    const CompositionEpoch adopted = composed.compositionEpoch;
    auto changedLayout = layout;
    changedLayout[1].opacity = 0.5f;
    check(coordinator.updateLayout(changedLayout) == LayoutUpdateResult::Updated,
          "opacity snapshot を変更");
    check(coordinator.compositionEpoch() == CompositionEpoch{2}, "opacity 変更で epoch +1");
    check(composed.compositionEpoch == adopted, "採用済み epoch は immutable");
    check(coordinator.validateForDisplay(composed) == CompositionResult::StaleEpoch,
          "old CompositionEpoch を拒否");

    auto movedLayout = changedLayout;
    movedLayout[1].destination.x = 0.25f;
    check(coordinator.updateLayout(movedLayout) == LayoutUpdateResult::Updated,
          "destination snapshot を変更");
    check(coordinator.compositionEpoch() == CompositionEpoch{3}, "destination 変更で epoch +1");

    checkNear(straightAlphaBlend(0.8f, 0.2f, 0.0f), 0.2, 1e-6, "opacity 0");
    checkNear(straightAlphaBlend(0.8f, 0.2f, 0.5f), 0.5, 1e-6, "opacity 0.5");
    checkNear(straightAlphaBlend(0.8f, 0.2f, 1.0f), 0.8, 1e-6, "opacity 1");

    int releasedA = 0;
    int releasedB = 0;
    fa.lifetime = FrameLifetimeToken(&releasedA, [](void* p) { ++*static_cast<int*>(p); });
    fb.lifetime = FrameLifetimeToken(&releasedB, [](void* p) { ++*static_cast<int*>(p); });
    check(coordinator.compose(10, {fa, fb}, composed) == CompositionResult::Accepted,
          "aggregate 用 composition");
    GpuRetirementQueue retirement;
    retirement.retire(20, aggregateLifetime(composed));
    composed = {};
    fa.lifetime.reset();
    fb.lifetime.reset();
    checkEq(static_cast<long long>(retirement.poll(19)), 0, "serial 完了前は aggregate を保持");
    checkEq(releasedA, 0, "serial 完了前は A を解放しない");
    checkEq(releasedB, 0, "serial 完了前は B を解放しない");
    checkEq(static_cast<long long>(retirement.poll(20)), 1, "serial 完了で aggregate を解放");
    checkEq(releasedA, 1, "A を aggregate と同時に解放");
    checkEq(releasedB, 1, "B を aggregate と同時に解放");

    WorkerJoinBarrier joins(2);
    check(!joins.allJoined(), "worker running 中は teardown 不可");
    check(joins.noteJoined(0), "worker A join");
    check(!joins.allJoined(), "A だけの join では teardown 不可");
    check(joins.noteJoined(1), "worker B join");
    check(joins.allJoined(), "両 worker join 後だけ teardown 可");

    SourceFrameBuffer bounded(a, SourceGeneration{2}, 0);
    auto bounded0 = makeFrame(0, 2);
    bounded0.sourceId = a;
    auto bounded1 = makeFrame(1, 2);
    bounded1.sourceId = a;
    check(bounded.submitFrame(bounded0) == SubmitResult::Accepted,
          "capacity 0 は最小 capacity 1 として 1 frame を受理");
    check(bounded.submitFrame(bounded1) == SubmitResult::RejectedQueueFull,
          "overflow は暗黙 drop せず結果を返す");
    checkEq(static_cast<long long>(bounded.depth()), 1, "bounded buffer は capacity を超えない");

    std::atomic<int> waiting{0};
    bool waiterResults[2] = {true, true};
    auto waitForStop = [&](int index) {
        waiting.fetch_add(1, std::memory_order_release);
        waiterResults[index] = bounded.waitForSpace(10000);
    };
    std::thread waiterA(waitForStop, 0);
    std::thread waiterB(waitForStop, 1);
    while (waiting.load(std::memory_order_acquire) != 2)
        std::this_thread::yield();
    bounded.stop();
    waiterA.join();
    waiterB.join();
    check(!waiterResults[0] && !waiterResults[1], "stop ですべての waiter が起床");

    PreviewFrameQueue pending(4);
    check(pending.registerSource(a, SourceGeneration{2}), "pending 検査で A を登録");
    check(pending.registerSource(b, SourceGeneration{7}), "pending 検査で B を登録");
    auto pendingA = makeFrame(10, 2);
    pendingA.sourceId = a;
    auto pendingB = makeFrame(10, 7);
    pendingB.sourceId = b;
    check(pending.submitFrame(pendingA) == SubmitResult::Accepted, "A pending を追加");
    check(pending.submitFrame(pendingB) == SubmitResult::Accepted, "B pending を追加");
    check(pending.unregisterSource(a), "A unregister で A pending だけ除去");
    checkEq(static_cast<long long>(pending.depth()), 1, "B pending は維持");
    DecodedGpuFrame remaining;
    check(pending.takeForDisplay(remaining) && remaining.sourceId == b,
          "unregister 後に残る pending は B");

    SourceFrameBuffer exactA(a, SourceGeneration{2}, 4);
    SourceFrameBuffer exactB(b, SourceGeneration{7}, 4);
    auto oldA = makeFrame(9, 2);
    oldA.sourceId = a;
    auto exactFrameA = makeFrame(10, 2);
    exactFrameA.sourceId = a;
    auto oldFrameB = makeFrame(8, 7);
    oldFrameB.sourceId = b;
    auto exactFrameB = makeFrame(10, 7);
    exactFrameB.sourceId = b;
    check(exactA.submitFrame(oldA) == SubmitResult::Accepted, "A stale候補を追加");
    check(exactA.submitFrame(exactFrameA) == SubmitResult::Accepted, "A exact候補を追加");
    check(exactB.submitFrame(oldFrameB) == SubmitResult::Accepted, "B stale候補を追加");
    check(exactB.submitFrame(exactFrameB) == SubmitResult::Accepted, "B exact候補を追加");
    ExactFramePairer pairer(exactA, exactB, coordinator);
    check(pairer.tryPair(10, composed) == PairResult::Paired, "staleを除去してexact pairを形成");
    checkEq(pairer.counters().staleADiscardCount, 1, "A stale discardを計数");
    checkEq(pairer.counters().staleBDiscardCount, 1, "B stale discardを計数");
    checkEq(pairer.counters().pairedCount, 1, "exact pairを計数");

    auto futureFrameA = makeFrame(12, 2);
    futureFrameA.sourceId = a;
    auto requestedFrameB = makeFrame(11, 7);
    requestedFrameB.sourceId = b;
    check(exactA.submitFrame(futureFrameA) == SubmitResult::Accepted, "A future frameを追加");
    check(exactB.submitFrame(requestedFrameB) == SubmitResult::Accepted, "B requested frameを追加");
    check(pairer.tryPair(11, composed) == PairResult::MissingA,
          "A future/B exactをmixed pairにしない");
    SourceFrameIdentity keptA;
    SourceFrameIdentity keptB;
    check(exactA.peekFrontIdentity(keptA) && keptA.frameNumber == 12, "A future frameを消費しない");
    check(exactB.peekFrontIdentity(keptB) && keptB.frameNumber == 11,
          "片方だけ成立したB frameを消費しない");
    checkEq(pairer.counters().missingACount, 1, "missing Aを計数");

    // A の確認後、commit 前に B の generation が変わった状況を決定論的に再現する。
    SourceFrameBuffer transactionalA(a, SourceGeneration{2}, 2);
    SourceFrameBuffer transactionalB(b, SourceGeneration{7}, 2);
    auto keptFrameA = makeFrame(20, 2);
    keptFrameA.sourceId = a;
    auto changedFrameB = makeFrame(20, 7);
    changedFrameB.sourceId = b;
    check(transactionalA.submitFrame(keptFrameA) == SubmitResult::Accepted,
          "transactional A を追加");
    check(transactionalB.submitFrame(changedFrameB) == SubmitResult::Accepted,
          "transactional B を追加");
    SourceFrameIdentity confirmedA;
    check(transactionalA.peekFrontIdentity(confirmedA) && confirmedA.frameNumber == 20,
          "commit 前に A exact を確認");
    check(transactionalB.setGeneration(SourceGeneration{8}) == GenerationUpdateResult::Updated,
          "A確認後にB generation changeを再現");
    DecodedGpuFrame notTakenA;
    DecodedGpuFrame notTakenB;
    check(
        !SourceFrameBuffer::takeExactPair(transactionalA, transactionalB, 20, notTakenA, notTakenB),
        "片側変更時はpair commitを拒否");
    check(transactionalA.peekFrontIdentity(confirmedA) && confirmedA.frameNumber == 20,
          "pair失敗でAを失わない");
    checkEq(pairer.counters().partialPairConsumeCount, 0, "partial pair consume は 0");
}

// --------------------------------------------------------------------------
// event query の serial state machine (P1.2 §4)
// --------------------------------------------------------------------------
// **実 GPU では再現させにくい経路**なので、純粋な状態機械として検査する。
// 実機で event query backend を通した検証はしていない (実機未検証)。
void testEventQueryLedger() {
    std::fprintf(stderr, "[event query ledger]\n");

    // slot は不透明な識別子。ここでは int の番地を使う。
    int slots[4] = {0, 1, 2, 3};

    { // slot が無ければ serial を確定しない (追跡できないものを進めない)
        EventQueryLedger l(2);
        checkEq(static_cast<long long>(l.confirmSubmission(nullptr)), 0,
                "slot 無しでは serial を確定しない");
        checkEq(static_cast<long long>(l.submittedSerial()), 0, "submitted も進まない");
    }

    { // 確保 -> 確定 -> 完了で serial が進む
        EventQueryLedger l(4);
        l.addSlot(&slots[0]);
        l.addSlot(&slots[1]);
        checkEq(static_cast<long long>(l.totalSlots()), 2, "slot 2 個");

        auto* s0 = l.acquireFreeSlot();
        check(s0 != nullptr, "slot を確保できる");
        const unsigned long long serial0 = l.confirmSubmission(s0);
        checkEq(static_cast<long long>(serial0), 1, "最初の serial は 1");

        auto* s1 = l.acquireFreeSlot();
        const unsigned long long serial1 = l.confirmSubmission(s1);
        checkEq(static_cast<long long>(serial1), 2, "次の serial は 2");
        checkEq(static_cast<long long>(l.inFlight()), 2, "2 件が in-flight");

        // 先頭が未完了なら completed は進まない (**terminal serial を飛ばさない**)
        check(l.advanceCompleted([&](void* s) { return s == s1 ? 1 : 0; }), "poll は成功");
        checkEq(static_cast<long long>(l.completedSerial()), 0,
                "先頭が未完了なら後ろが完了していても進めない");

        // 先頭が完了すれば 1 まで進む
        check(l.advanceCompleted([&](void* s) { return s == s0 ? 1 : 0; }), "poll は成功");
        checkEq(static_cast<long long>(l.completedSerial()), 1, "先頭の完了で 1 まで");

        // 残りも完了すれば 2 まで
        check(l.advanceCompleted([](void*) { return 1; }), "poll は成功");
        checkEq(static_cast<long long>(l.completedSerial()), 2, "全部完了で 2 まで");

        // 完了した slot は再利用される
        checkEq(static_cast<long long>(l.inFlight()), 0, "in-flight は 0");
        check(l.acquireFreeSlot() != nullptr, "完了した slot を再利用できる");
    }

    { // 上限に達したら nullptr を返す (fail-closed 側は呼び出し元が判断)
        EventQueryLedger l(1);
        l.addSlot(&slots[0]);
        auto* s0 = l.acquireFreeSlot();
        l.confirmSubmission(s0);
        check(l.acquireFreeSlot() == nullptr, "空きが無ければ nullptr");
        check(l.atCapacity(), "上限に達している");
    }

    { // GetData が失敗したら completed を進めない (fail-closed)
        EventQueryLedger l(4);
        l.addSlot(&slots[0]);
        auto* s0 = l.acquireFreeSlot();
        l.confirmSubmission(s0);
        check(!l.advanceCompleted([](void*) { return -1; }), "失敗は false を返す");
        checkEq(static_cast<long long>(l.completedSerial()), 0, "失敗時に completed を進めない");
    }

    { // takeAllSlots で使用中・未使用の両方を回収できる (解放漏れを防ぐ)
        EventQueryLedger l(4);
        l.addSlot(&slots[0]);
        l.addSlot(&slots[1]);
        auto* s0 = l.acquireFreeSlot();
        l.confirmSubmission(s0);
        checkEq(static_cast<long long>(l.takeAllSlots().size()), 2, "全 slot を回収する");
        checkEq(static_cast<long long>(l.totalSlots()), 0, "回収後は空");
    }
}

void testRetirementQueue() {
    std::fprintf(stderr, "[retirement queue]\n");

    GpuRetirementQueue rq;
    int a = 0, b = 0, c = 0;
    auto tok = [](int* p) {
        *p = 0;
        return FrameLifetimeToken(p, [](void* x) { *static_cast<int*>(x) = 1; });
    };

    rq.retire(10, tok(&a));
    rq.retire(20, tok(&b));
    rq.retire(30, tok(&c));
    checkEq(static_cast<long long>(rq.depthCurrent()), 3, "3 件保持");
    checkEq(static_cast<long long>(rq.depthPeak()), 3, "peak 3");

    // completed=5: まだ何も完了していない
    checkEq(static_cast<long long>(rq.poll(5)), 0, "未完了は解放しない");
    checkEq(a, 0, "serial 10 は未完了");

    // completed=20: serial 10 と 20 が解放される
    checkEq(static_cast<long long>(rq.poll(20)), 2, "10 と 20 を解放");
    checkEq(a, 1, "serial 10 解放");
    checkEq(b, 1, "serial 20 解放");
    checkEq(c, 0, "serial 30 は未完了のまま保持");
    checkEq(static_cast<long long>(rq.depthCurrent()), 1, "残り 1 件");

    // completed=100: 残りも解放
    checkEq(static_cast<long long>(rq.poll(100)), 1, "30 を解放");
    checkEq(c, 1, "serial 30 解放");
    checkEq(static_cast<long long>(rq.depthCurrent()), 0, "空");

    // **frames_released_before_completion は必ず 0**
    checkEq(rq.payloadsReleasedBeforeCompletion(), 0, "完了前解放は 0");

    { // drain: completed が追いつけば true
        GpuRetirementQueue r2;
        int x = 0;
        r2.retire(50, tok(&x));
        unsigned long long completed = 0;
        // 呼ばれるたびに completed が進む擬似 GPU
        check(r2.drain(
                  [&completed] {
                      completed += 25;
                      return completed;
                  },
                  1000),
              "completed が追いつけば drain 成功");
        checkEq(x, 1, "drain で解放");
    }
    { // drain: 追いつかなければ timeout で false (fail-closed)
        GpuRetirementQueue r3;
        int y = 0;
        r3.retire(1000000, tok(&y));
        check(!r3.drain([] { return 0ULL; }, 30), "完了しなければ timeout で false");
        checkEq(r3.retirementTimeoutCount(), 1, "timeout を数える");
        checkEq(y, 0, "timeout では強制解放しない (payload は保持)");
    }
}

void testCompositionDisplayLedger() {
    std::fprintf(stderr, "[composition display ledger]\n");
    CompositionDisplayLedger ledger(2);
    auto a = makeFrame(10, 3);
    a.sourceId = {1};
    a.resourceEpoch = {7};
    auto b = makeFrame(10, 4);
    b.sourceId = {2};
    b.resourceEpoch = {8};
    ComposedFrame frame{10, {9}, {{a, {}, {}, 1.0f, 0}, {b, {}, {}, 0.75f, 1}}};

    const auto before = ledger.baseline();
    checkEq(static_cast<long long>(before), 0, "初期baseline");
    ledger.record(frame, 100);
    CompositionDisplayExpectation expected{10, {9}, {identityOf(a), identityOf(b)}};
    CompositionDisplayRecord found;
    check(ledger.findAfter(before, expected, found), "baseline後の完全identity一致");
    check(ledger.findEpochAfter(before, CompositionEpoch{9}, found), "baseline後の要求epoch一致");
    check(!ledger.findEpochAfter(before, CompositionEpoch{8}, found), "old epochを拒否");
    checkEq(static_cast<long long>(found.displaySequence), 1, "sequenceは単調増加");

    auto wrong = expected;
    wrong.sources[1].sourceGeneration = {99};
    check(!ledger.findAfter(before, wrong, found), "一方のgeneration違いを拒否");
    check(!ledger.findAfter(ledger.baseline(), expected, found), "arm前displayを成功にしない");

    frame.outputFrameNumber = 11;
    ledger.record(frame, 101);
    frame.outputFrameNumber = 12;
    ledger.record(frame, 102);
    checkEq(static_cast<long long>(ledger.size()), 2, "historyを上限内に保つ");
}

void testOutputScheduler() {
    std::fprintf(stderr, "[output scheduler]\n");
    OutputScheduler60Hz scheduler;
    scheduler.start(1000, 60000);
    checkEq(scheduler.next().deadlineQpc, 1000, "frame 0 deadline");
    checkEq(scheduler.next().deadlineQpc, 2000, "frame 1 deadline");
    scheduler.start(1000, 60000);
    const auto notDue = scheduler.takeDue(999);
    check(!notDue.due, "deadline前はscheduleしない");
    const auto skipped = scheduler.takeDue(3050);
    check(skipped.due, "deadline到達でscheduleする");
    checkEq(skipped.output.outputFrameNumber, 2, "missしたdeadline後のexact output frame");
    checkEq(skipped.skippedDeadlineCount, 2, "missしたdeadlineを個別に数える");
    check(
        OutputScheduler60Hz::classifyDeadline(PairResult::MissingA, CompositionResult::Accepted) ==
            OutputDropReason::MissingSourceA,
        "missing A分類");
    check(
        OutputScheduler60Hz::classifyDeadline(PairResult::MissingB, CompositionResult::Accepted) ==
            OutputDropReason::MissingSourceB,
        "missing B分類");
    check(OutputScheduler60Hz::classifyDeadline(PairResult::StaleGeneration,
                                                CompositionResult::Accepted) ==
              OutputDropReason::StaleGeneration,
          "stale generation分類");
    check(
        OutputScheduler60Hz::classifyDeadline(PairResult::Paired, CompositionResult::StaleEpoch) ==
            OutputDropReason::StaleCompositionEpoch,
        "stale epoch分類");
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
    testDisplayLedger();
    testDeviceChangeOrder();
    testLifecycleOrder();
    testCompletionFatalStopsRendering();
    testSourceLifecycle();
    testCompositionAdoptionEpoch();
    testP2SourceAndComposition();
    testEventQueryLedger();
    testGenerationContract();
    testRetirementQueue();
    testCompositionDisplayLedger();
    testOutputScheduler();
    testHwFormatSelection();

    std::fprintf(stderr, "\n検査 %d 件 / 失敗 %d 件\n", gChecks, gFailures);
    if (gChecks == 0) {
        std::fprintf(stderr, "検査が 1 件も実行されていません\n");
        return 1;
    }
    return gFailures == 0 ? 0 : 1;
}
