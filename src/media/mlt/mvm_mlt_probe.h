/*
 * mvm Phase 0 / S4 - MLT 経由のメディア解析とフレーム取り出し (V2)
 *
 * 位置づけ:
 *   まだ IMediaEngine ではない。V2 (素材読み込み) を判定するために必要な
 *   最小限だけを持つ。MLT の型はこのヘッダに一切出さない。
 *
 * 目的:
 *   「MLT が FFmpeg 8 に対してビルドされている」ことと
 *   「各 codec を正しく decode できる」ことは別問題である。
 *   後者をここで実測する。
 */

#ifndef MVM_MLT_PROBE_H
#define MVM_MLT_PROBE_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int ok;
    char error[512];

    int has_video;
    int has_audio;

    char video_codec[64];
    char audio_codec[64];
    char pix_fmt[64];

    int width;
    int height;

    /* fps は有理数のまま持つ。小数へ潰すと 60000/1001 のような素材で壊れる。 */
    int fps_num;
    int fps_den;

    int sar_num;
    int sar_den;

    long long frame_count;
    double duration_sec;

    /* MLT の frame_count は「profile の fps でのフレーム数」である。
     * 音声のみの素材には映像 fps が無いため、profile の fps がそのまま効く。
     * duration を出すにはこちらを使う必要がある。 */
    int profile_fps_num;
    int profile_fps_den;

    /* [S4 の所見] MLT は静止画の length を INT_MAX として返す。
     * 「尺が無限」という意味であり、duration として扱ってはいけない。
     * 気づかずに使うと 8.6e7 秒という値がタイムラインへ流れ込む。 */
    int is_unbounded_length;

    int sample_rate;
    int channels;

    /* alpha は「pix_fmt にアルファがある」だけでなく、実際に取り出した
     * フレームのアルファ値が一定でないことまで見て判定する。 */
    int has_alpha;
    int alpha_min;
    int alpha_max;
} MvmMltProbeResult;

/*
 * 素材を解析する。path は UTF-8。
 *
 * MLT の profile は素材から導出する (mlt_profile_from_producer)。
 * 固定 profile のまま読むと length や fps が profile 側に正規化され、
 * ffprobe と比較しても意味のない値になる。
 *
 * 戻り値: 0 = 成功
 */
int mvm_mlt_probe_file(const char* path, MvmMltProbeResult* out);

/*
 * producer の全プロパティを出力する。
 * MLT のプロパティ名は version やモジュールによって変わるため、
 * 推測ではなく実測で確認できるようにしておく。
 */
int mvm_mlt_dump_properties(const char* path, FILE* out);

typedef struct {
    int width;
    int height;
    unsigned char* rgba; /* width*height*4。mvm_mlt_image_free で解放する */
} MvmMltImage;

/*
 * 指定フレームを RGBA で取り出す。
 *
 * 戻り値: 0 = 成功
 */
int mvm_mlt_decode_frame(const char* path, long long frame, MvmMltImage* out, char* err,
                         size_t err_size);

void mvm_mlt_image_free(MvmMltImage* img);

#ifdef __cplusplus
}
#endif

#endif /* MVM_MLT_PROBE_H */
