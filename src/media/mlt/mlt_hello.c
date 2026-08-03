/*
 * mvm Phase 0 / S1 - MLT hello world
 *
 * 目的:
 *   prebuilt MLT (MSYS2 UCRT64) が「リンクできる」だけでなく
 *   「実行時にモジュールを実際にロードできる」ことまで確認する。
 *
 *   MLT はモジュールを実行時に動的ロードするため、リンク成功は動作を保証しない。
 *   モジュールの解決に失敗しても MLT は静かに縮退し、後の工程で
 *   「producer が見つからない」という分かりにくい形で表面化する。
 *   その事故を S1 の時点で潰しておく。
 *
 * 制約:
 *   このファイルは src/media/mlt/ 配下にある。mvm では MLT のヘッダを
 *   include してよいのはこのディレクトリだけであり、Mlt++ (C++ ラッパ) は
 *   使わず C API (mlt_*) のみを使う。
 *
 * 終了コード:
 *   0 = 必須モジュールが全て解決できた
 *   1 = MLT の初期化に失敗
 *   2 = 必須モジュールが不足
 */

#include <framework/mlt.h>

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MLT はモジュールの dlopen に失敗しても stderr に 1 行出すだけで処理を続ける。
 * 失敗したモジュール一覧を API から取得する手段は無い。そこで自前でモジュール
 * ディレクトリを走査し、ロードできない DLL を明示的に洗い出す。
 *
 * これは V11 (clean 環境への配置) でそのまま必要になる検査でもある。
 * staging で依存 DLL を 1 つ落とすと、同じ「存在するのに使えない」状態になり、
 * VM 上では原因の分からない機能欠落として現れるためである。
 *
 * 戻り値: ロードに失敗したモジュール数 */
static int report_unloadable_modules(const char* module_dir)
{
    printf("\n[module dlopen check] %s\n", module_dir);

    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.dll", module_dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        printf("  !! モジュールディレクトリを走査できません\n");
        return 0;
    }

    int total = 0, failed = 0;
    do {
        char full[MAX_PATH];
        snprintf(full, sizeof(full), "%s\\%s", module_dir, fd.cFileName);
        total++;

        HMODULE m = LoadLibraryA(full);
        if (m) {
            FreeLibrary(m);
        } else {
            /* 大半は「依存 DLL が無い」(ERROR_MOD_NOT_FOUND = 126)。
             * どの DLL が足りないかは ldd で特定できる。 */
            printf("  FAIL %-28s (Win32 error %lu)\n", fd.cFileName, GetLastError());
            failed++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (failed == 0) {
        printf("  OK   %d モジュール全てロードできました\n", total);
    } else {
        printf("  %d/%d モジュールがロードできません。\n", failed, total);
        printf("  依存 DLL が不足しています。特定するには MSYS2 UCRT64 シェルで:\n");
        printf("    ldd %s/<module>.dll | grep 'not found'\n", module_dir);
    }
    return failed;
}

/* Phase 0 の検証項目 V2/V3/V7/V8 で必要になるモジュール。
 * ここで足りないものが分かれば、MLT のビルド構成の問題を S1 で検知できる。 */
static const char* kRequiredProducers[] = {
    "avformat", /* H.264 / HEVC / WAV : V2 */
    "qimage",   /* PNG (alpha)        : V2 */
    "xml",      /* デバッグ用ダンプの読み戻し */
    "color",    /* 単色生成 (テスト用背景) */
    NULL,
};

static const char* kRequiredFilters[] = {
    /* MLT は avfilter を "avfilter.<name>" という名前で個別に登録する。
     * "avfilter" という名前の filter は存在しない。 */
    "avfilter.scale",
    "affine",       /* transform / scale : V7 (plus モジュール) */
    "crop",         /* crop              : V7 */
    "brightness",   /* opacity / fade    : V7 */
    "volume",       /* audio gain        : V7 */
    "dynamictext",  /* 文字レイヤ        : V3 (plus モジュール) */
    "qtext",        /* 文字レイヤ (Qt 描画。日本語のシェーピングに必要) : V3 */
    NULL,
};

static const char* kRequiredTransitions[] = {
    "qtblend",   /* 映像合成 : V3 */
    "mix",       /* 音声合成 : V3 */
    "composite",
    NULL,
};

static const char* kRequiredConsumers[] = {
    "avformat", /* 書き出し           : V8 */
    "sdl2",     /* preview (目視確認) : V5 */
    "null",     /* 計測               : V5 */
    NULL,
};

static int has_service(mlt_properties list, const char* name)
{
    int count = mlt_properties_count(list);
    for (int i = 0; i < count; i++) {
        const char* got = mlt_properties_get_name(list, i);
        if (got && strcmp(got, name) == 0)
            return 1;
    }
    return 0;
}

/* 一覧を出しつつ必須分を照合し、不足数を返す。 */
static int report_category(const char* label, mlt_properties list, const char* const* required)
{
    printf("\n[%s] %d 件\n", label, list ? mlt_properties_count(list) : 0);

    if (!list) {
        printf("  !! 一覧を取得できませんでした\n");
        int n = 0;
        for (const char* const* r = required; *r; ++r)
            n++;
        return n;
    }

    /* 全量は多いので、必須のものだけを明示的に照合する */
    int missing = 0;
    for (const char* const* r = required; *r; ++r) {
        if (has_service(list, *r)) {
            printf("  OK   %s\n", *r);
        } else {
            printf("  FAIL %s  <-- 必須モジュールが見つかりません\n", *r);
            missing++;
        }
    }
    return missing;
}

int main(int argc, char** argv)
{
    printf("=== mvm Phase 0 / S1 : MLT hello world ===\n");

#ifdef MVM_MLT_VERSION_STRING
    printf("build 時の pkg-config version : %s\n", MVM_MLT_VERSION_STRING);
#endif

    /* [S1 の所見 1]
     * mlt_factory_init(NULL) は、モジュールとデータの場所を「実行ファイルからの
     * 相対」で推測する。開発ビルドでは実行ファイルが build/<preset>/bin に
     * あるため推測は必ず外れ、build/<preset>/lib/mlt を探して何も見つけられない。
     *
     * このとき MLT は失敗せず、モジュール 0 件のまま静かに縮退する。
     * 後の工程では「producer が見つからない」という原因の分かりにくい形でしか
     * 表面化しない。よって場所は常に明示的に与える。
     *
     * V11 (staging) では、ここを実行ファイル相対の同梱パスに差し替える。
     * 環境変数には依存させない (ユーザー環境の MLT_REPOSITORY に引きずられる
     * 事故を避けるため、自プロセス内で設定する)。 */
    const char* module_dir = (argc > 1) ? argv[1] : MVM_MLT_MODULE_DIR;
    const char* data_dir = (argc > 2) ? argv[2] : MVM_MLT_DATA_DIR;

    printf("module dir (指定)            : %s\n", module_dir);
    printf("data dir   (指定)            : %s\n", data_dir);

    /* MLT_DATA は factory 初期化前に設定されている必要がある
     * (profiles/ の解決に使われる)。 */
    if (_putenv_s("MLT_DATA", data_dir) != 0) {
        fprintf(stderr, "!! MLT_DATA を設定できませんでした\n");
        return 1;
    }

    mlt_repository repo = mlt_factory_init(module_dir);
    if (!repo) {
        fprintf(stderr,
                "!! mlt_factory_init に失敗しました。\n"
                "   モジュールディレクトリ: %s\n",
                module_dir);
        return 1;
    }

    printf("実行時 MLT version           : %s\n", mlt_version_get_string());
    printf("MLT_REPOSITORY               : %s\n",
           mlt_environment("MLT_REPOSITORY") ? mlt_environment("MLT_REPOSITORY") : "(未設定)");
    printf("MLT_DATA                     : %s\n",
           mlt_environment("MLT_DATA") ? mlt_environment("MLT_DATA") : "(未設定)");

    int missing = 0;
    missing += report_unloadable_modules(module_dir);
    missing += report_category("producers", mlt_repository_producers(repo), kRequiredProducers);
    missing += report_category("filters", mlt_repository_filters(repo), kRequiredFilters);
    missing += report_category("transitions", mlt_repository_transitions(repo), kRequiredTransitions);
    missing += report_category("consumers", mlt_repository_consumers(repo), kRequiredConsumers);

    /* [S1 の所見 2]
     * mlt_profile_init() は、指定した profile 名が解決できなくても NULL を
     * 返さない。既定値 (dv_pal 相当の 720x576 @ 25fps) に静かにフォールバックする。
     * つまり「非 NULL が返ったこと」は成功を意味しない。
     *
     * これは mvm にとって危険な挙動である。profile は解像度・fps・SAR を決め、
     * V12 (preview と final render の一致) の前提そのものだからである。
     * 取り違えたまま進むと、原因の分からない尺ずれ・幾何ずれとして表面化する。
     *
     * よって profile は必ず「返ってきた値」を検証する。 */
    printf("\n[profile]\n");
    {
        const char* kProfileName = "atsc_1080p_60";
        const int kWantW = 1920, kWantH = 1080, kWantFpsNum = 60, kWantFpsDen = 1;

        mlt_profile profile = mlt_profile_init(kProfileName);
        if (!profile) {
            printf("  FAIL %s : mlt_profile_init が NULL を返しました\n", kProfileName);
            missing++;
        } else {
            printf("  ---  %s : %dx%d @ %d/%d fps (SAR %d/%d)\n", kProfileName, profile->width,
                   profile->height, profile->frame_rate_num, profile->frame_rate_den,
                   profile->sample_aspect_num, profile->sample_aspect_den);

            if (profile->width == kWantW && profile->height == kWantH
                && profile->frame_rate_num == kWantFpsNum
                && profile->frame_rate_den == kWantFpsDen) {
                printf("  OK   期待値と一致しました\n");
            } else {
                printf("  FAIL 期待値 %dx%d @ %d/%d と一致しません。\n", kWantW, kWantH,
                       kWantFpsNum, kWantFpsDen);
                printf("       profile 定義ファイルが解決できず、既定値へ\n");
                printf("       フォールバックしています。MLT_DATA を確認してください:\n");
                printf("       期待: %s/profiles/%s\n", data_dir, kProfileName);
                missing++;
            }
            mlt_profile_close(profile);
        }
    }

    mlt_factory_close();

    printf("\n=== 結果 ===\n");
    if (missing == 0) {
        printf("必須モジュールは全て解決できました。S1 の MLT 一次確認は成功です。\n");
        return 0;
    }
    printf("%d 件の必須項目が不足しています。MLT のビルド構成を確認してください。\n", missing);
    return 2;
}
