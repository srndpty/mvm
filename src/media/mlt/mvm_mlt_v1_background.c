#include "mvm_mlt_v1_background.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MVM_V1_BACKGROUND_PROPERTY "_mvm_v1_background"

struct MvmV1Background {
    mlt_producer producer;
    int (*original_get_frame)(mlt_producer, mlt_frame_ptr, int);
    int width, height;
    int guarded;          /* 0 なら検査しない (保護なしの対照) */
    const uint8_t* cache; /* prewarm のときの cache の buffer (比べるだけで、読まない) */
    int cache_size;
    volatile LONG64 frames;
    volatile LONG64 violations;
    volatile LONG first_ready;
    char first[240]; /* 最初の違反 (first_ready が非 0 になってから読む) */
};

static void background_error(char* err, size_t err_size, const char* message) {
    if (err && err_size > 0)
        snprintf(err, err_size, "%s", message);
}

/* rgba の全画素が黒 (0, 0, 0, 255) か。 */
static int all_opaque_black(const uint8_t* rgba, size_t pixels) {
    for (size_t index = 0; index < pixels; ++index) {
        const uint8_t* pixel = rgba + index * 4;
        if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 255)
            return 0;
    }
    return 1;
}

static int opaque_black_at(const uint8_t* rgba, size_t pixel) {
    const uint8_t* value = rgba + pixel * 4;
    return value[0] == 0 && value[1] == 0 && value[2] == 0 && value[3] == 255;
}

/* 最初の違反だけを文にする。数は全部数える。 */
static void record_violation(MvmV1Background* background, long long position, const char* what,
                             int format, int width, int height) {
    if (InterlockedIncrement64(&background->violations) == 1) {
        snprintf(background->first, sizeof(background->first),
                 "V1 の背景: %s (frame %lld、形式 %d、%dx%d。期待は rgba の %dx%d)", what, position,
                 format, width, height, background->width, background->height);
        InterlockedExchange(&background->first_ready, 1);
    }
}

/* color producer の画像の要求の後に、cache が prewarm のときのままであることを確かめる。
 * render worker から並行に呼ばれる。 */
static int guarded_get_image(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width,
                             int* height, int writable) {
    mlt_producer producer = (mlt_producer)mlt_frame_pop_service(frame);
    mlt_properties properties = MLT_PRODUCER_PROPERTIES(producer);
    MvmV1Background* background =
        (MvmV1Background*)mlt_properties_get_data(properties, MVM_V1_BACKGROUND_PROPERTY, NULL);
    const long long position = (long long)mlt_frame_get_position(frame);
    const int error = mlt_frame_get_image(frame, image, format, width, height, writable);
    if (!background)
        return error;
    InterlockedIncrement64(&background->frames);
    int cache_size = 0;
    const uint8_t* cache =
        (const uint8_t*)mlt_properties_get_data(properties, "image", &cache_size);
    const size_t pixels = (size_t)background->width * (size_t)background->height;
    if (error || !image || !*image) {
        record_violation(background, position, "画像を取得できません", (int)*format, *width,
                         *height);
    } else if (*format != mlt_image_rgba || *width != background->width ||
               *height != background->height) {
        record_violation(background, position, "出力と違う形の画像が返りました", (int)*format,
                         *width, *height);
    } else if (cache != background->cache || cache_size != background->cache_size ||
               mlt_properties_get_int(properties, "_format") != (int)mlt_image_rgba ||
               mlt_properties_get_int(properties, "_width") != background->width ||
               mlt_properties_get_int(properties, "_height") != background->height) {
        /* 作り直しの間に来た worker は、埋まる前の画素を受け取っていることがある。 */
        record_violation(background, position, "cache が prewarm の後で作り直されました",
                         mlt_properties_get_int(properties, "_format"),
                         mlt_properties_get_int(properties, "_width"),
                         mlt_properties_get_int(properties, "_height"));
    } else if (!opaque_black_at(*image, 0) || !opaque_black_at(*image, pixels / 2) ||
               !opaque_black_at(*image, pixels - 1)) {
        record_violation(background, position, "画素が黒 (alpha 255) ではありません", (int)*format,
                         *width, *height);
    }
    return error;
}

static int guarded_get_frame(mlt_producer producer, mlt_frame_ptr frame, int index) {
    MvmV1Background* background = (MvmV1Background*)mlt_properties_get_data(
        MLT_PRODUCER_PROPERTIES(producer), MVM_V1_BACKGROUND_PROPERTY, NULL);
    if (!background || !background->original_get_frame)
        return 1;
    const int status = background->original_get_frame(producer, frame, index);
    if (status == 0 && frame && *frame) {
        /* producer が積んだ画像の要求の上に、検査を積む。 */
        mlt_frame_push_service(*frame, producer);
        mlt_frame_push_get_image(*frame, guarded_get_image);
    }
    return status;
}

/* 1 thread で画像を 1 回要求して cache を作らせ、cache の全画素を確かめる。0 で成功。 */
static int prewarm(MvmV1Background* background, mlt_image_format request, int verify, char* err,
                   size_t err_size) {
    mlt_producer producer = background->producer;
    mlt_frame frame = NULL;
    if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(producer), &frame, 0) != 0 || !frame) {
        background_error(err, err_size, "V1 の背景の frame を作れません");
        return 1;
    }
    uint8_t* image = NULL;
    mlt_image_format format = request;
    int width = background->width, height = background->height;
    const int error = mlt_frame_get_image(frame, &image, &format, &width, &height, 0);
    int failed = 0;
    if (verify) {
        const size_t pixels = (size_t)background->width * (size_t)background->height;
        mlt_properties properties = MLT_PRODUCER_PROPERTIES(producer);
        int cache_size = 0;
        const uint8_t* cache =
            (const uint8_t*)mlt_properties_get_data(properties, "image", &cache_size);
        if (error || !image || format != mlt_image_rgba || width != background->width ||
            height != background->height) {
            background_error(err, err_size,
                             "V1 の背景が、出力と同じ大きさの rgba の画像を返しません");
            failed = 1;
        } else if (!cache || (size_t)cache_size != pixels * 4 ||
                   mlt_properties_get_int(properties, "_format") != (int)mlt_image_rgba ||
                   mlt_properties_get_int(properties, "_width") != background->width ||
                   mlt_properties_get_int(properties, "_height") != background->height) {
            background_error(err, err_size, "V1 の背景の cache が期待の形で作られていません");
            failed = 1;
        } else if (!all_opaque_black(image, pixels) || !all_opaque_black(cache, pixels)) {
            background_error(err, err_size, "V1 の背景の画素が黒 (alpha 255) ではありません");
            failed = 1;
        } else {
            background->cache = cache;
            background->cache_size = cache_size;
        }
    }
    mlt_frame_close(frame);
    /* frame を作ると producer の位置が進む。元へ戻す。 */
    mlt_producer_seek(producer, 0);
    return failed;
}

MvmV1Background* mvm_mlt_v1_background_new(mlt_profile profile, long long length, int width,
                                           int height, int fault, char* err, size_t err_size) {
    if (!profile || length <= 0 || width <= 0 || height <= 0) {
        background_error(err, err_size, "V1 の背景の入力が不正です");
        return NULL;
    }
    MvmV1Background* background = (MvmV1Background*)calloc(1, sizeof(*background));
    if (!background) {
        background_error(err, err_size, "V1 の背景の状態を確保できません");
        return NULL;
    }
    background->width = width;
    background->height = height;
    background->producer = mlt_factory_producer(profile, "color", "#000000");
    if (!background->producer) {
        free(background);
        background_error(err, err_size, "V1 の背景 (color producer) を作れません");
        return NULL;
    }
    mlt_producer producer = background->producer;
    mlt_properties properties = MLT_PRODUCER_PROPERTIES(producer);
    mlt_properties_set_position(properties, "length", (mlt_position)length + 1);
    mlt_properties_set_position(properties, "out", (mlt_position)length);
    if (fault == MVM_V1_BACKGROUND_FAULT_NO_PREWARM)
        return background; /* 保護なし (検出器の対照)。検査もしない */
    /* 形式を固定する。要求側の形式 (overlay の無い frame では consumer の yuv) に関わらず、
     * producer は rgba で作る。固定しないと、要求の形式が変わるたびに cache が作り直される。 */
    if (fault != MVM_V1_BACKGROUND_FAULT_NO_FORMAT)
        mlt_properties_set(properties, "mlt_image_format", "rgba");
    if (prewarm(background, mlt_image_rgba, 1, err, err_size) != 0 ||
        fault == MVM_V1_BACKGROUND_FAULT_PREWARM_FAILS) {
        if (fault == MVM_V1_BACKGROUND_FAULT_PREWARM_FAILS)
            background_error(err, err_size, "V1 の背景の prewarm に失敗しました (故障の注入)");
        mlt_producer_close(producer);
        free(background);
        return NULL;
    }
    /* 以降にこの producer が作る frame は、画像の要求ごとに cache を確かめる。 */
    if (mlt_properties_set_data(properties, MVM_V1_BACKGROUND_PROPERTY, background, 0, NULL,
                                NULL) != 0) {
        mlt_producer_close(producer);
        free(background);
        background_error(err, err_size, "V1 の背景の検査を接続できません");
        return NULL;
    }
    background->original_get_frame = producer->get_frame;
    producer->get_frame = guarded_get_frame;
    background->guarded = 1;
    if (fault == MVM_V1_BACKGROUND_FAULT_REBUILD) {
        /* 対照: prewarm の後で cache を作り直させる。色の文字列を、同じ色の別の書き方へ変える
         * (MLT は文字列が違えば作り直す)。作り直した画像は、形式も大きさも画素も prewarm のときと
         * 同じなので、検出できるのは「cache の buffer が変わった」ことだけである。
         * 要求は検査つきの道を通す (実際の書き出しでは、どの要求も検査を通る)。 */
        mlt_properties_set(properties, "resource", "0x000000ff");
        mlt_frame frame = NULL;
        if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(producer), &frame, 0) == 0 && frame) {
            uint8_t* image = NULL;
            mlt_image_format format = mlt_image_rgba;
            int request_width = width, request_height = height;
            mlt_frame_get_image(frame, &image, &format, &request_width, &request_height, 0);
            mlt_frame_close(frame);
        }
        mlt_producer_seek(producer, 0);
    }
    return background;
}

mlt_producer mvm_mlt_v1_background_producer(MvmV1Background* background) {
    return background ? background->producer : NULL;
}

int mvm_mlt_v1_background_failed(MvmV1Background* background, char* err, size_t err_size) {
    if (!background || InterlockedCompareExchange64(&background->violations, 0, 0) == 0)
        return 0;
    if (InterlockedCompareExchange(&background->first_ready, 0, 0))
        background_error(err, err_size, background->first);
    else
        background_error(err, err_size, "V1 の背景の検査に失敗しました");
    return 1;
}

void mvm_mlt_v1_background_stats(MvmV1Background* background, long long* frames,
                                 long long* violations) {
    if (frames)
        *frames =
            background ? (long long)InterlockedCompareExchange64(&background->frames, 0, 0) : 0;
    if (violations)
        *violations =
            background ? (long long)InterlockedCompareExchange64(&background->violations, 0, 0) : 0;
}

void mvm_mlt_v1_background_close(MvmV1Background* background) {
    if (!background)
        return;
    if (background->guarded) {
        background->producer->get_frame = background->original_get_frame;
        mlt_properties_set_data(MLT_PRODUCER_PROPERTIES(background->producer),
                                MVM_V1_BACKGROUND_PROPERTY, NULL, 0, NULL, NULL);
    }
    mlt_producer_close(background->producer);
    free(background);
}
