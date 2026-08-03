/*
 * mvm Phase 0 - MLT ランタイム初期化と健全性検査 (doctor)
 *
 * 位置づけ:
 *   S1 で判明した「MLT は失敗しても失敗と分からない」性質への対策を、
 *   mvm_mlt_hello と mvm_bench の両方から使えるように切り出したもの。
 *   まだ IMediaEngine ではない。Phase 0 の判定に必要な最小限だけを持つ。
 *
 * 検出対象 (S1 所見):
 *   1. モジュールディレクトリの指定が外れていても mlt_factory_init は成功する
 *      -> モジュール 0 件のまま静かに縮退する
 *   2. mlt_profile_init は profile が解決できなくても NULL を返さず、
 *      既定値 (720x576@25) へ黙ってフォールバックする
 *   3. モジュール DLL は存在しても依存 DLL 不足で dlopen できないことがある
 *      -> MLT は stderr に 1 行出すだけで処理を継続する
 *
 * 制約:
 *   MLT のヘッダを include してよいのは src/media/mlt/ 配下だけなので、
 *   このヘッダは MLT の型を一切公開しない。呼び出し側は MLT を知らなくてよい。
 */

#ifndef MVM_MLT_RUNTIME_H
#define MVM_MLT_RUNTIME_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MVM_MLT_MAX_ISSUES 128

typedef struct {
    char subject[256]; /* 何が (モジュール名 / service 名 / profile 名) */
    char detail[512];  /* なぜ */
} MvmMltIssue;

typedef struct {
    /* いずれも呼び出し側が所有する文字列を借用する */
    const char* module_dir;
    const char* data_dir;
    const char* mlt_version;

    int modules_total;  /* 走査できた *.dll の数 */
    int modules_failed; /* LoadLibraryW に失敗した数。走査自体の失敗も 1 と数える */

    int services_checked;
    int services_missing;
    int producers_count;
    int filters_count;
    int transitions_count;
    int consumers_count;

    int profile_checked;
    int profile_ok;
    int profile_width;
    int profile_height;
    int profile_fps_num;
    int profile_fps_den;
    int profile_sar_num;
    int profile_sar_den;

    int issue_count;
    MvmMltIssue issues[MVM_MLT_MAX_ISSUES];
} MvmMltDoctorReport;

/*
 * MLT を初期化する。module_dir / data_dir は UTF-8。
 * どちらも必須で、NULL や空文字は拒否する。
 *
 * MLT に場所を推測させない (S1 所見 1)。推測は開発ビルドでは必ず外れ、
 * V11 の staging では機能欠落として現れるため。
 *
 * 戻り値: 0 = 成功、非 0 = 失敗
 */
int mvm_mlt_runtime_init(const char* module_dir, const char* data_dir);

void mvm_mlt_runtime_shutdown(void);

/* 初期化済みかどうか */
int mvm_mlt_runtime_is_ready(void);

/*
 * モジュールディレクトリを走査し、LoadLibraryW できない DLL を洗い出す。
 *
 * 走査できない / DLL が 1 つも無い場合も失敗として数える。
 * 「空のディレクトリを指していた」は MLT が最も静かに縮退するケースであり、
 * 成功扱いにしてはならない。
 *
 * mvm_mlt_runtime_init より前でも呼べる (純粋な Win32 の検査であるため)。
 *
 * 戻り値: 失敗したモジュール数 (0 = 全てロード可能)
 */
int mvm_mlt_scan_modules(const char* module_dir, MvmMltDoctorReport* report);

/*
 * 健全性検査をまとめて実行する。mvm_mlt_runtime_init の後に呼ぶ。
 *
 * profile_name とその期待値 (want_*) を渡すと、返ってきた profile の
 * 実値と照合する。値が一致しなければ失敗として数える (S1 所見 2)。
 * want_* に 0 を渡した項目は照合しない。
 *
 * SAR も照合対象に含める。SAR は幾何の解釈を決めるため、取り違えると
 * V12 (preview と final の一致) で「なぜか横に伸びる」という形で表面化する。
 *
 * 戻り値: 問題の総数 (0 = 健全)
 */
int mvm_mlt_doctor_run(const char* profile_name, int want_w, int want_h, int want_fps_num,
                       int want_fps_den, int want_sar_num, int want_sar_den,
                       MvmMltDoctorReport* report);

/* 人間向けのレポートを出力する */
void mvm_mlt_doctor_print(const MvmMltDoctorReport* report, FILE* out);

/* 機械可読な 1 行サマリ。復元検証スクリプト等から grep される。 */
void mvm_mlt_doctor_print_summary_line(const MvmMltDoctorReport* report, FILE* out);

/* JSON で出力する (mvm_bench doctor --json 用) */
void mvm_mlt_doctor_print_json(const MvmMltDoctorReport* report, FILE* out);

/* Phase 0 で必須としている service 一覧 (診断メッセージ用に公開) */
extern const char* const* mvm_mlt_required_producers(void);
extern const char* const* mvm_mlt_required_filters(void);
extern const char* const* mvm_mlt_required_transitions(void);
extern const char* const* mvm_mlt_required_consumers(void);

#ifdef __cplusplus
}
#endif

#endif /* MVM_MLT_RUNTIME_H */
