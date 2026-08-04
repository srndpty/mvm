#include "mvm_mlt_runtime.h"

#include "../../util/mvm_win_utf8.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <framework/mlt.h>

/* ------------------------------------------------------------------------- */
/* 必須 service                                                               */
/* ------------------------------------------------------------------------- */

static const char* const kRequiredProducers[] = {
    "avformat", /* H.264 / HEVC / WAV : V2 */
    "qimage",   /* PNG (alpha)        : V2 */
    "xml",      /* デバッグ用ダンプの読み戻し */
    "color",    /* 単色生成 (テスト用背景) */
    NULL,
};

static const char* const kRequiredFilters[] = {
    /* MLT は avfilter を "avfilter.<name>" として個別に登録する。
     * "avfilter" という名前の filter は存在しない (S1 所見 5)。 */
    "avfilter.scale",
    "affine",      /* transform / scale : V7 (plus モジュール) */
    "crop",        /* crop              : V7 */
    "brightness",  /* opacity / fade    : V7 */
    "volume",      /* audio gain        : V7 */
    "dynamictext", /* 文字レイヤ        : V3 (plus モジュール) */
    "qtext",       /* 文字レイヤ (Qt 描画。日本語のシェーピングに必要) : V3 */
    NULL,
};

static const char* const kRequiredTransitions[] = {
    "qtblend", /* 映像合成 : V3 */
    "mix",     /* 音声合成 : V3 */
    "composite",
    NULL,
};

static const char* const kRequiredConsumers[] = {
    "avformat", /* 書き出し           : V8 */
    "sdl2",     /* preview (目視確認) : V5 */
    "null",     /* 計測               : V5 */
    NULL,
};

const char* const* mvm_mlt_required_producers(void) {
    return kRequiredProducers;
}

const char* const* mvm_mlt_required_filters(void) {
    return kRequiredFilters;
}

const char* const* mvm_mlt_required_transitions(void) {
    return kRequiredTransitions;
}

const char* const* mvm_mlt_required_consumers(void) {
    return kRequiredConsumers;
}

/* ------------------------------------------------------------------------- */
/* 内部状態                                                                   */
/* ------------------------------------------------------------------------- */

static mlt_repository g_repo = NULL;
static int g_ready = 0;

int mvm_mlt_runtime_is_ready(void) {
    return g_ready;
}

/* ------------------------------------------------------------------------- */
/* issue 記録                                                                 */
/* ------------------------------------------------------------------------- */

static void add_issue(MvmMltDoctorReport* r, const char* subject, const char* detail) {
    if (!r || r->issue_count >= MVM_MLT_MAX_ISSUES)
        return;

    MvmMltIssue* it = &r->issues[r->issue_count++];
    snprintf(it->subject, sizeof(it->subject), "%s", subject ? subject : "");
    snprintf(it->detail, sizeof(it->detail), "%s", detail ? detail : "");
}

/* ------------------------------------------------------------------------- */
/* 初期化                                                                     */
/* ------------------------------------------------------------------------- */

int mvm_mlt_runtime_init(const char* module_dir, const char* data_dir) {
    if (g_ready)
        return 0;

    /* 場所は必ず明示させる。NULL を許すと MLT の推測に落ちてしまい、
     * S1 所見 1 の静かな縮退を再現してしまう。 */
    if (!module_dir || !*module_dir) {
        fprintf(stderr, "[mvm] module_dir が指定されていません\n");
        return 1;
    }
    if (!data_dir || !*data_dir) {
        fprintf(stderr, "[mvm] data_dir が指定されていません\n");
        return 1;
    }

    /* [V10 の実測所見]
     * MLT はモジュール/データディレクトリを内部で ANSI (*A) API として扱う。
     * UTF-8 の非 ASCII パスを渡すと 1 つも解決できず、しかも
     * mlt_factory_init は成功を返す (service 0 件のまま静かに縮退する)。
     *
     * これらのディレクトリは mvm 自身が配置するもの (V11 staging) なので
     * ASCII に保てる。ユーザーが選ぶ素材ファイルのパスとは別問題であり、
     * そちらは producer の resource として別途検証する必要がある。
     *
     * ここでは「気づかないまま縮退する」ことだけは防ぐ。 */
    for (const char* p = module_dir; *p; ++p) {
        if ((unsigned char)*p >= 0x80) {
            fprintf(stderr,
                    "[mvm] 警告: module_dir に非 ASCII 文字が含まれています。\n"
                    "      MLT はこれを解決できず、service 0 件のまま縮退します。\n"
                    "      module_dir は ASCII のみのパスに配置してください: %s\n",
                    module_dir);
            break;
        }
    }
    for (const char* p = data_dir; *p; ++p) {
        if ((unsigned char)*p >= 0x80) {
            fprintf(stderr,
                    "[mvm] 警告: data_dir に非 ASCII 文字が含まれています。\n"
                    "      profile が解決できず既定値へフォールバックする可能性があります: %s\n",
                    data_dir);
            break;
        }
    }

    /* MLT_DATA は factory 初期化前に設定されている必要がある
     * (profiles/ の解決に使われる)。 */
    if (_putenv_s("MLT_DATA", data_dir) != 0) {
        fprintf(stderr, "[mvm] MLT_DATA を設定できませんでした: %s\n", data_dir);
        return 1;
    }

    g_repo = mlt_factory_init(module_dir);
    if (!g_repo) {
        fprintf(stderr, "[mvm] mlt_factory_init に失敗しました: %s\n", module_dir);
        return 1;
    }

    g_ready = 1;
    return 0;
}

void mvm_mlt_runtime_shutdown(void) {
    if (g_ready) {
        mlt_factory_close();
        g_repo = NULL;
        g_ready = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* モジュール走査 (wide API)                                                  */
/* ------------------------------------------------------------------------- */

int mvm_mlt_scan_modules(const char* module_dir, MvmMltDoctorReport* report) {
    MvmMltDoctorReport dummy;
    if (!report) {
        memset(&dummy, 0, sizeof(dummy));
        report = &dummy;
    }

    report->modules_total = 0;
    report->modules_failed = 0;

    if (!module_dir || !*module_dir) {
        add_issue(report, "(module_dir)", "モジュールディレクトリが指定されていません");
        report->modules_failed = 1;
        return 1;
    }

    /* UTF-8 -> wide。日本語を含むパスでも走査できるようにする (V10)。 */
    wchar_t* wdir = mvm_utf8_to_wide(module_dir);
    if (!wdir) {
        add_issue(report, module_dir, "パスを UTF-16 へ変換できません (不正な UTF-8)");
        report->modules_failed = 1;
        return 1;
    }

    wchar_t* pattern = mvm_wide_concat(wdir, L"\\*.dll");
    if (!pattern) {
        mvm_str_free(wdir);
        add_issue(report, module_dir, "検索パターンを構築できません (メモリ不足)");
        report->modules_failed = 1;
        return 1;
    }

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        char* msg = mvm_win_error_message(err);
        char detail[512];

        if (err == ERROR_FILE_NOT_FOUND) {
            /* ディレクトリはあるが DLL が 1 つも無い、という場合もここに来る。
             * これは MLT が最も静かに縮退するケースなので必ず失敗にする。 */
            snprintf(detail, sizeof(detail),
                     "モジュール DLL が 1 つも見つかりません。"
                     "MLT はこの状態でも初期化に成功し、service 0 件のまま縮退します。");
        } else {
            snprintf(detail, sizeof(detail), "走査できません (Win32 error %lu: %s)",
                     (unsigned long)err, msg ? msg : "");
        }
        mvm_str_free(msg);
        add_issue(report, module_dir, detail);

        mvm_str_free(pattern);
        mvm_str_free(wdir);
        report->modules_failed = 1;
        return 1;
    }

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        wchar_t* sep = mvm_wide_concat(wdir, L"\\");
        wchar_t* full = sep ? mvm_wide_concat(sep, fd.cFileName) : NULL;
        mvm_str_free(sep);

        if (!full) {
            report->modules_failed++;
            add_issue(report, "(unknown)", "フルパスを構築できません (メモリ不足)");
            continue;
        }

        report->modules_total++;

        HMODULE m = LoadLibraryW(full);
        if (m) {
            FreeLibrary(m);
        } else {
            DWORD err = GetLastError();
            char* msg = mvm_win_error_message(err);
            char* name = mvm_wide_to_utf8(fd.cFileName);
            char detail[512];

            /* 数値だけでは原因が分からない。ERROR_MOD_NOT_FOUND (126) は
             * ほぼ常に「依存 DLL が足りない」であり、S1 所見 3 の再来である。 */
            snprintf(detail, sizeof(detail), "LoadLibraryW 失敗 (Win32 error %lu: %s)%s",
                     (unsigned long)err, msg ? msg : "",
                     err == ERROR_MOD_NOT_FOUND ? " -- 依存 DLL 不足の可能性が高い" : "");

            add_issue(report, name ? name : "(unknown)", detail);
            report->modules_failed++;

            mvm_str_free(name);
            mvm_str_free(msg);
        }
        mvm_str_free(full);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    mvm_str_free(pattern);
    mvm_str_free(wdir);

    if (report->modules_total == 0) {
        add_issue(report, module_dir, "モジュール DLL が 0 件です");
        report->modules_failed++;
    }

    return report->modules_failed;
}

/* ------------------------------------------------------------------------- */
/* service 照合                                                               */
/* ------------------------------------------------------------------------- */

static int has_service(mlt_properties list, const char* name) {
    if (!list)
        return 0;
    int count = mlt_properties_count(list);
    for (int i = 0; i < count; i++) {
        const char* got = mlt_properties_get_name(list, i);
        if (got && strcmp(got, name) == 0)
            return 1;
    }
    return 0;
}

static int check_category(MvmMltDoctorReport* r, const char* label, mlt_properties list,
                          const char* const* required, int* out_count) {
    *out_count = list ? mlt_properties_count(list) : 0;

    int missing = 0;
    for (const char* const* n = required; *n; ++n) {
        r->services_checked++;
        if (!has_service(list, *n)) {
            char detail[512];
            snprintf(detail, sizeof(detail), "必須 %s が見つかりません (登録数 %d)", label,
                     *out_count);
            add_issue(r, *n, detail);
            missing++;
        }
    }
    return missing;
}

/* ------------------------------------------------------------------------- */
/* doctor                                                                     */
/* ------------------------------------------------------------------------- */

/* 有理数を gcd で正規化する。SAR は 1/1 と 2/2 のような等価表現があるため、
 * 数値をそのまま比較してはいけない。 */
static void normalize_ratio(int* num, int* den) {
    if (*den == 0)
        return;
    int a = *num < 0 ? -*num : *num;
    int b = *den < 0 ? -*den : *den;
    while (b != 0) {
        int t = a % b;
        a = b;
        b = t;
    }
    if (a > 0) {
        *num /= a;
        *den /= a;
    }
    if (*den < 0) {
        *num = -*num;
        *den = -*den;
    }
}

int mvm_mlt_doctor_run(const char* profile_name, int want_w, int want_h, int want_fps_num,
                       int want_fps_den, int want_sar_num, int want_sar_den,
                       MvmMltDoctorReport* report) {
    if (!report)
        return -1;

    if (!g_ready) {
        add_issue(report, "(runtime)", "mvm_mlt_runtime_init が呼ばれていません");
        return 1;
    }

    report->mlt_version = mlt_version_get_string();

    /* 1. モジュールが実際にロードできるか (S1 所見 3) */
    mvm_mlt_scan_modules(report->module_dir, report);

    /* 2. 必須 service が登録されているか */
    report->services_missing = 0;
    report->services_missing += check_category(report, "producer", mlt_repository_producers(g_repo),
                                               kRequiredProducers, &report->producers_count);
    report->services_missing += check_category(report, "filter", mlt_repository_filters(g_repo),
                                               kRequiredFilters, &report->filters_count);
    report->services_missing +=
        check_category(report, "transition", mlt_repository_transitions(g_repo),
                       kRequiredTransitions, &report->transitions_count);
    report->services_missing += check_category(report, "consumer", mlt_repository_consumers(g_repo),
                                               kRequiredConsumers, &report->consumers_count);

    /* 3. profile の「返ってきた値」を検証する (S1 所見 2)
     *
     * mlt_profile_init は解決に失敗しても NULL を返さず、既定値
     * (720x576 @ 25/1) へ黙ってフォールバックする。非 NULL は成功を意味しない。 */
    if (profile_name && *profile_name) {
        report->profile_checked = 1;
        mlt_profile p = mlt_profile_init(profile_name);
        if (!p) {
            add_issue(report, profile_name, "mlt_profile_init が NULL を返しました");
        } else {
            report->profile_width = p->width;
            report->profile_height = p->height;
            report->profile_fps_num = p->frame_rate_num;
            report->profile_fps_den = p->frame_rate_den;
            report->profile_sar_num = p->sample_aspect_num;
            report->profile_sar_den = p->sample_aspect_den;

            int ok = 1;
            if (want_w && p->width != want_w)
                ok = 0;
            if (want_h && p->height != want_h)
                ok = 0;
            if (want_fps_num && p->frame_rate_num != want_fps_num)
                ok = 0;
            if (want_fps_den && p->frame_rate_den != want_fps_den)
                ok = 0;

            /* SAR は等価な表現 (1/1 と 2/2) があるので、正規化してから比較する。 */
            if (want_sar_num && want_sar_den) {
                int gn = p->sample_aspect_num, gd = p->sample_aspect_den;
                int wn = want_sar_num, wd = want_sar_den;
                normalize_ratio(&gn, &gd);
                normalize_ratio(&wn, &wd);
                if (gn != wn || gd != wd)
                    ok = 0;
            }

            report->profile_ok = ok;
            if (!ok) {
                char detail[512];
                snprintf(detail, sizeof(detail),
                         "期待 %dx%d @ %d/%d SAR %d/%d に対し実値 %dx%d @ %d/%d SAR %d/%d。"
                         "profile 定義が解決できず既定値へフォールバックしている可能性があります "
                         "(MLT_DATA=%s/profiles を確認)",
                         want_w, want_h, want_fps_num, want_fps_den, want_sar_num, want_sar_den,
                         p->width, p->height, p->frame_rate_num, p->frame_rate_den,
                         p->sample_aspect_num, p->sample_aspect_den,
                         report->data_dir ? report->data_dir : "(未設定)");
                add_issue(report, profile_name, detail);
            }
            mlt_profile_close(p);
        }
    }

    return report->issue_count;
}

/* ------------------------------------------------------------------------- */
/* 出力                                                                       */
/* ------------------------------------------------------------------------- */

void mvm_mlt_doctor_print(const MvmMltDoctorReport* r, FILE* out) {
    if (!r || !out)
        return;

    fprintf(out, "MLT version   : %s\n", r->mlt_version ? r->mlt_version : "(不明)");
    fprintf(out, "module dir    : %s\n", r->module_dir ? r->module_dir : "(未指定)");
    fprintf(out, "data dir      : %s\n", r->data_dir ? r->data_dir : "(未指定)");

    fprintf(out, "\n[modules] %d 件中 %d 件がロード失敗\n", r->modules_total, r->modules_failed);
    fprintf(out, "[services] producers=%d filters=%d transitions=%d consumers=%d\n",
            r->producers_count, r->filters_count, r->transitions_count, r->consumers_count);
    fprintf(out, "           必須 %d 件中 %d 件が欠落\n", r->services_checked, r->services_missing);

    if (r->profile_checked) {
        fprintf(out, "[profile]  %dx%d @ %d/%d fps (SAR %d/%d) -> %s\n", r->profile_width,
                r->profile_height, r->profile_fps_num, r->profile_fps_den, r->profile_sar_num,
                r->profile_sar_den, r->profile_ok ? "期待値と一致" : "期待値と不一致");
    }

    if (r->issue_count > 0) {
        fprintf(out, "\n[問題 %d 件]\n", r->issue_count);
        for (int i = 0; i < r->issue_count; i++) {
            fprintf(out, "  FAIL %s\n       %s\n", r->issues[i].subject, r->issues[i].detail);
        }
    } else {
        fprintf(out, "\n問題は検出されませんでした。\n");
    }
}

void mvm_mlt_doctor_print_summary_line(const MvmMltDoctorReport* r, FILE* out) {
    if (!r || !out)
        return;

    /* 検証スクリプトから grep される安定した 1 行。書式を変えないこと。 */
    fprintf(out,
            "MVM_DOCTOR_RESULT modules_total=%d modules_failed=%d services_checked=%d "
            "services_missing=%d profile_ok=%d profile=%dx%d@%d/%d sar=%d/%d issues=%d\n",
            r->modules_total, r->modules_failed, r->services_checked, r->services_missing,
            r->profile_ok, r->profile_width, r->profile_height, r->profile_fps_num,
            r->profile_fps_den, r->profile_sar_num, r->profile_sar_den, r->issue_count);
}

static void json_escape(const char* s, FILE* out) {
    fputc('"', out);
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; ++p) {
        switch (*p) {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (*p < 0x20)
                fprintf(out, "\\u%04x", *p);
            else
                fputc((int)*p, out);
        }
    }
    fputc('"', out);
}

void mvm_mlt_doctor_print_json(const MvmMltDoctorReport* r, FILE* out) {
    if (!r || !out)
        return;

    fputs("{\n", out);
    fputs("  \"mlt_version\": ", out);
    json_escape(r->mlt_version, out);
    fputs(",\n  \"module_dir\": ", out);
    json_escape(r->module_dir, out);
    fputs(",\n  \"data_dir\": ", out);
    json_escape(r->data_dir, out);

    fprintf(out,
            ",\n  \"modules\": { \"total\": %d, \"failed\": %d },"
            "\n  \"services\": { \"producers\": %d, \"filters\": %d, \"transitions\": %d,"
            " \"consumers\": %d, \"required_checked\": %d, \"required_missing\": %d },"
            "\n  \"profile\": { \"checked\": %d, \"ok\": %d, \"width\": %d, \"height\": %d,"
            " \"fps_num\": %d, \"fps_den\": %d, \"sar_num\": %d, \"sar_den\": %d }",
            r->modules_total, r->modules_failed, r->producers_count, r->filters_count,
            r->transitions_count, r->consumers_count, r->services_checked, r->services_missing,
            r->profile_checked, r->profile_ok, r->profile_width, r->profile_height,
            r->profile_fps_num, r->profile_fps_den, r->profile_sar_num, r->profile_sar_den);

    fputs(",\n  \"issues\": [", out);
    for (int i = 0; i < r->issue_count; i++) {
        fputs(i ? ",\n    " : "\n    ", out);
        fputs("{ \"subject\": ", out);
        json_escape(r->issues[i].subject, out);
        fputs(", \"detail\": ", out);
        json_escape(r->issues[i].detail, out);
        fputs(" }", out);
    }
    fputs(r->issue_count ? "\n  ]\n}\n" : "]\n}\n", out);
}
