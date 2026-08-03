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
 *
 * 検査ロジックの実体は mvm_mlt_runtime.c にある。
 * mvm_bench doctor と同じ部品を使うことで、両者が食い違わないようにしている。
 *
 * 用途:
 *   - 開発機での素早い確認
 *   - R0 (凍結パッケージ復元検証) で、復元先の MLT を検査する
 *     例: mvm_mlt_hello.exe <restored>/lib/mlt <restored>/share/mlt
 *
 * 終了コード:
 *   0 = 健全
 *   1 = MLT の初期化に失敗
 *   2 = 検査で問題を検出
 */

#include "mvm_mlt_runtime.h"

#include "../../util/mvm_win_utf8.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    /* FormatMessageW 由来の日本語メッセージや日本語パスを化けさせない */
    mvm_enable_utf8_console();

    /* main の argv は ANSI であり UTF-8 ではない。日本語パスを正しく受け取るため
     * GetCommandLineW から取り直す (V10 の実測所見)。 */
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv) {
        fprintf(stderr, "!! コマンドライン引数を取得できませんでした\n");
        return 1;
    }

    printf("=== mvm Phase 0 : MLT hello world ===\n");

#ifdef MVM_MLT_VERSION_STRING
    printf("build 時の pkg-config version : %s\n", MVM_MLT_VERSION_STRING);
#endif

    /* 場所は常に明示的に与える。MLT に推測させると、開発ビルドでは必ず外れ、
     * モジュール 0 件のまま静かに初期化に成功してしまう (S1 所見 1)。
     *
     * argv で上書きできるのは、R0 の復元検証で「復元先の MLT」を
     * 検査する必要があるため。 */
    const char* module_dir = (argc > 1) ? argv[1] : MVM_MLT_MODULE_DIR;
    const char* data_dir = (argc > 2) ? argv[2] : MVM_MLT_DATA_DIR;

    printf("module dir (指定)             : %s\n", module_dir);
    printf("data dir   (指定)             : %s\n", data_dir);
    printf("\n");

    if (mvm_mlt_runtime_init(module_dir, data_dir) != 0) {
        fprintf(stderr, "!! MLT の初期化に失敗しました\n");
        mvm_win_free_utf8_args(argv, argc);
        return 1;
    }

    MvmMltDoctorReport report;
    memset(&report, 0, sizeof(report));
    report.module_dir = module_dir;
    report.data_dir = data_dir;

    /* atsc_1080p_60 の実値まで照合する。非 NULL が返ったことは
     * 成功を意味しない (S1 所見 2)。 */
    int issues = mvm_mlt_doctor_run("atsc_1080p_60", 1920, 1080, 60, 1, &report);

    mvm_mlt_doctor_print(&report, stdout);
    mvm_mlt_doctor_print_summary_line(&report, stdout);

    mvm_mlt_runtime_shutdown();
    mvm_win_free_utf8_args(argv, argc);

    if (issues == 0) {
        printf("\n結果: 健全です。\n");
        return 0;
    }
    printf("\n結果: %d 件の問題を検出しました。\n", issues);
    return 2;
}
