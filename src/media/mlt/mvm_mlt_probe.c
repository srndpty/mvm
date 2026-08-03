#include "mvm_mlt_probe.h"

#include "mvm_mlt_runtime.h"

#include <framework/mlt.h>

#include <stdlib.h>
#include <string.h>

static void set_err(char* err, size_t n, const char* msg)
{
    if (err && n)
        snprintf(err, n, "%s", msg);
}

/* profile は素材から導出する。
 *
 * 固定 profile のまま avformat producer を読むと、length や frame_rate が
 * profile 側に正規化される。その値を ffprobe と比較しても
 * 「MLT が素材をどう解釈したか」ではなく「profile を何にしたか」しか分からない。 */
static mlt_producer open_producer(const char* path, mlt_profile* out_profile)
{
    mlt_profile profile = mlt_profile_init(NULL);
    if (!profile)
        return NULL;

    mlt_producer probe = mlt_factory_producer(profile, "avformat", path);
    if (!probe) {
        mlt_profile_close(profile);
        return NULL;
    }

    /* 素材の解像度・fps・SAR を profile へ取り込む */
    mlt_profile_from_producer(profile, probe);
    mlt_producer_close(probe);

    /* 取り込んだ profile で開き直す */
    mlt_producer p = mlt_factory_producer(profile, "avformat", path);
    if (!p) {
        mlt_profile_close(profile);
        return NULL;
    }

    *out_profile = profile;
    return p;
}

int mvm_mlt_dump_properties(const char* path, FILE* out)
{
    if (!mvm_mlt_runtime_is_ready()) {
        fprintf(out, "!! MLT が初期化されていません\n");
        return 1;
    }

    mlt_profile profile = NULL;
    mlt_producer p = open_producer(path, &profile);
    if (!p) {
        fprintf(out, "!! producer を開けません: %s\n", path);
        if (profile)
            mlt_profile_close(profile);
        return 1;
    }

    mlt_properties props = MLT_PRODUCER_PROPERTIES(p);
    int n = mlt_properties_count(props);
    fprintf(out, "=== producer properties (%d) : %s ===\n", n, path);
    for (int i = 0; i < n; i++) {
        const char* name = mlt_properties_get_name(props, i);
        const char* value = mlt_properties_get_value(props, i);
        fprintf(out, "  %-44s = %s\n", name ? name : "(null)", value ? value : "(null)");
    }

    fprintf(out, "\n=== profile ===\n");
    fprintf(out, "  %dx%d @ %d/%d fps  SAR %d/%d  display %d/%d  progressive=%d colorspace=%d\n",
            profile->width, profile->height, profile->frame_rate_num, profile->frame_rate_den,
            profile->sample_aspect_num, profile->sample_aspect_den, profile->display_aspect_num,
            profile->display_aspect_den, profile->progressive, profile->colorspace);

    fprintf(out, "\n=== producer geometry ===\n");
    fprintf(out, "  length=%d in=%d out=%d\n", (int) mlt_producer_get_length(p),
            (int) mlt_producer_get_in(p), (int) mlt_producer_get_out(p));

    mlt_producer_close(p);
    mlt_profile_close(profile);
    return 0;
}

/* meta.media.<n>.stream.type が target のストリームを探す */
static int find_stream(mlt_properties props, const char* target)
{
    int nb = mlt_properties_get_int(props, "meta.media.nb_streams");
    for (int i = 0; i < nb; i++) {
        char key[128];
        snprintf(key, sizeof(key), "meta.media.%d.stream.type", i);
        const char* type = mlt_properties_get(props, key);
        if (type && strcmp(type, target) == 0)
            return i;
    }
    return -1;
}

static void copy_prop(char* dst, size_t n, mlt_properties props, const char* key)
{
    const char* v = mlt_properties_get(props, key);
    snprintf(dst, n, "%s", v ? v : "");
}

int mvm_mlt_probe_file(const char* path, MvmMltProbeResult* out)
{
    if (!out)
        return 1;
    memset(out, 0, sizeof(*out));

    if (!mvm_mlt_runtime_is_ready()) {
        set_err(out->error, sizeof(out->error), "MLT が初期化されていません");
        return 1;
    }
    if (!path || !*path) {
        set_err(out->error, sizeof(out->error), "パスが空です");
        return 1;
    }

    mlt_profile profile = NULL;
    mlt_producer p = open_producer(path, &profile);
    if (!p) {
        snprintf(out->error, sizeof(out->error), "producer を開けません: %s", path);
        if (profile)
            mlt_profile_close(profile);
        return 1;
    }

    mlt_properties props = MLT_PRODUCER_PROPERTIES(p);

    /* [実測] service に "avformat" を明示指定した場合、MLT は開けない素材に対して
     * mlt_factory_producer が NULL を返す。以下の 5 種で確認済み:
     *   0 バイト / ランダムバイト列 / 途中切断 mp4 / テキスト / 字幕のみ mp4
     * したがって現時点では、下の 2 つの検査はいずれも到達しない。
     *
     * それでも残すのは以下の理由による。
     *   - service を NULL (loader / 自動判定) にすると、MLT は未知の入力に対して
     *     別の producer へフォールバックしうる。その場合 producer は非 NULL で
     *     返り、length が 0 や 1 の「無音の黒 1 フレーム」として静かに流れる。
     *     mvm が将来 loader 経路を使うなら、この検査が唯一の防波堤になる。
     *   - コストがほぼゼロで、失敗を静かに通すリスクの方が高い。
     *
     * [未検証] loader 経路の実際のフォールバック挙動は測っていない。
     * service を可変にする時点で必ず確かめること。 */
    int nb_streams = mlt_properties_get_int(props, "meta.media.nb_streams");

    int vs = find_stream(props, "video");
    int as = find_stream(props, "audio");

    out->has_video = (vs >= 0);
    out->has_audio = (as >= 0);

    if (nb_streams <= 0) {
        snprintf(out->error, sizeof(out->error),
                 "ストリームを 1 つも解釈できません (meta.media.nb_streams=%d)。"
                 "素材が壊れているか、対応していない形式です",
                 nb_streams);
        mlt_producer_close(p);
        mlt_profile_close(profile);
        return 1;
    }

    if (!out->has_video && !out->has_audio) {
        snprintf(out->error, sizeof(out->error),
                 "映像ストリームも音声ストリームもありません (nb_streams=%d)。"
                 "素材が壊れている可能性があります",
                 nb_streams);
        mlt_producer_close(p);
        mlt_profile_close(profile);
        return 1;
    }

    if (vs >= 0) {
        char key[128];
        snprintf(key, sizeof(key), "meta.media.%d.codec.name", vs);
        copy_prop(out->video_codec, sizeof(out->video_codec), props, key);
        snprintf(key, sizeof(key), "meta.media.%d.codec.pix_fmt", vs);
        copy_prop(out->pix_fmt, sizeof(out->pix_fmt), props, key);

        out->width = mlt_properties_get_int(props, "meta.media.width");
        out->height = mlt_properties_get_int(props, "meta.media.height");

        out->fps_num = mlt_properties_get_int(props, "meta.media.frame_rate_num");
        out->fps_den = mlt_properties_get_int(props, "meta.media.frame_rate_den");
        out->sar_num = mlt_properties_get_int(props, "meta.media.sample_aspect_num");
        out->sar_den = mlt_properties_get_int(props, "meta.media.sample_aspect_den");

        /* meta.media が無い形式 (静止画など) では profile から拾う */
        if (out->width == 0)
            out->width = profile->width;
        if (out->height == 0)
            out->height = profile->height;
        if (out->fps_num == 0) {
            out->fps_num = profile->frame_rate_num;
            out->fps_den = profile->frame_rate_den;
        }
        if (out->sar_den == 0) {
            out->sar_num = profile->sample_aspect_num;
            out->sar_den = profile->sample_aspect_den;
        }
    }

    if (as >= 0) {
        char key[128];
        snprintf(key, sizeof(key), "meta.media.%d.codec.name", as);
        copy_prop(out->audio_codec, sizeof(out->audio_codec), props, key);
        snprintf(key, sizeof(key), "meta.media.%d.codec.sample_rate", as);
        out->sample_rate = mlt_properties_get_int(props, key);
        snprintf(key, sizeof(key), "meta.media.%d.codec.channels", as);
        out->channels = mlt_properties_get_int(props, key);
    }

    /* length は profile を素材から導出した後の値なので、素材のフレーム数に一致する。
     * ただし MLT の length は「最後のフレーム + 1」であり out = length - 1。 */
    out->frame_count = (long long) mlt_producer_get_length(p);
    out->profile_fps_num = profile->frame_rate_num;
    out->profile_fps_den = profile->frame_rate_den;

    /* [S4 の所見] MLT は静止画の length を INT_MAX で返す。
     * 「尺が無限」の意味であり duration ではない。そのまま秒に換算すると
     * 8.6e7 秒という値になる。呼び出し側が気づけるよう明示的に印を付ける。 */
    out->is_unbounded_length = (out->frame_count >= 0x7FFFFFFF) ? 1 : 0;

    /* duration は profile の fps で割る。素材の fps ではない。
     * 音声のみの素材には映像 fps が無く、length は profile の fps で
     * 数えられているため、素材 fps で割ると 0 になる。 */
    if (!out->is_unbounded_length && out->profile_fps_num > 0 && out->profile_fps_den > 0) {
        out->duration_sec = (double) out->frame_count * (double) out->profile_fps_den
                            / (double) out->profile_fps_num;
    }

    /* alpha は実際のフレームを見て判定する。pix_fmt 文字列だけでは
     * 「MLT のパイプラインを通った後もアルファが生きているか」が分からない。 */
    if (out->has_video) {
        mlt_frame frame = NULL;
        if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(p), &frame, 0) == 0 && frame) {
            mlt_image_format fmt = mlt_image_rgba;
            int w = 0, h = 0;
            uint8_t* image = NULL;
            if (mlt_frame_get_image(frame, &image, &fmt, &w, &h, 0) == 0 && image && w > 0
                && h > 0) {
                int amin = 255, amax = 0;
                for (int y = 0; y < h; y += 4) {
                    const uint8_t* row = image + (size_t) y * (size_t) w * 4;
                    for (int x = 0; x < w; x += 4) {
                        int a = row[(size_t) x * 4 + 3];
                        if (a < amin)
                            amin = a;
                        if (a > amax)
                            amax = a;
                    }
                }
                out->alpha_min = amin;
                out->alpha_max = amax;
                out->has_alpha = (amin < 255) ? 1 : 0;
            }
            mlt_frame_close(frame);
        }
    }

    out->ok = 1;
    mlt_producer_close(p);
    mlt_profile_close(profile);
    return 0;
}

int mvm_mlt_decode_frame(const char* path, long long frame, MvmMltImage* out, char* err,
                         size_t err_size)
{
    if (!out)
        return 1;
    memset(out, 0, sizeof(*out));

    if (!mvm_mlt_runtime_is_ready()) {
        set_err(err, err_size, "MLT が初期化されていません");
        return 1;
    }

    mlt_profile profile = NULL;
    mlt_producer p = open_producer(path, &profile);
    if (!p) {
        set_err(err, err_size, "producer を開けません");
        if (profile)
            mlt_profile_close(profile);
        return 1;
    }

    int length = (int) mlt_producer_get_length(p);
    if (frame < 0 || frame >= length) {
        char msg[256];
        snprintf(msg, sizeof(msg), "フレーム番号が範囲外です: %lld (length=%d)", frame, length);
        set_err(err, err_size, msg);
        mlt_producer_close(p);
        mlt_profile_close(profile);
        return 1;
    }

    mlt_producer_seek(p, (mlt_position) frame);

    mlt_frame f = NULL;
    if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(p), &f, 0) != 0 || !f) {
        set_err(err, err_size, "フレームを取得できません");
        mlt_producer_close(p);
        mlt_profile_close(profile);
        return 1;
    }

    mlt_image_format fmt = mlt_image_rgba;
    int w = 0, h = 0;
    uint8_t* image = NULL;
    if (mlt_frame_get_image(f, &image, &fmt, &w, &h, 0) != 0 || !image || w <= 0 || h <= 0) {
        set_err(err, err_size, "画像を取得できません");
        mlt_frame_close(f);
        mlt_producer_close(p);
        mlt_profile_close(profile);
        return 1;
    }

    size_t bytes = (size_t) w * (size_t) h * 4;
    out->rgba = (unsigned char*) malloc(bytes);
    if (!out->rgba) {
        set_err(err, err_size, "メモリを確保できません");
        mlt_frame_close(f);
        mlt_producer_close(p);
        mlt_profile_close(profile);
        return 1;
    }
    memcpy(out->rgba, image, bytes);
    out->width = w;
    out->height = h;

    mlt_frame_close(f);
    mlt_producer_close(p);
    mlt_profile_close(profile);
    return 0;
}

void mvm_mlt_image_free(MvmMltImage* img)
{
    if (!img)
        return;
    free(img->rgba);
    img->rgba = NULL;
    img->width = 0;
    img->height = 0;
}
