/*
 * mvm Phase 0 - Windows の UTF-8 / wide 文字列変換ヘルパ
 *
 * 位置づけ:
 *   これは Phase 0 のスパイク用ヘルパであり、製品用の platform 層ではない。
 *   Phase 0 が採用判定を出した後、必要なら src/platform/ として設計し直す。
 *   ここでは「日本語を含むパスを扱える」ことを検証できる最小限だけを持つ。
 *
 * なぜ必要か (V10):
 *   Windows の *A 系 API は ANSI コードページで解釈するため、日本語を含むパスを
 *   UTF-8 のまま渡すと壊れる。MAX_PATH (260) 固定長バッファも同様に事故源になる。
 *   mvm は内部を UTF-8 で統一し、Windows API を叩く直前で wide へ変換する。
 */

#ifndef MVM_WIN_UTF8_H
#define MVM_WIN_UTF8_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UTF-8 -> UTF-16。失敗時は NULL。戻り値は mvm_str_free() で解放する。 */
wchar_t* mvm_utf8_to_wide(const char* utf8);

/* UTF-16 -> UTF-8。失敗時は NULL。戻り値は mvm_str_free() で解放する。 */
char* mvm_wide_to_utf8(const wchar_t* wide);

/* 2 つの wide 文字列を連結する。失敗時は NULL。 */
wchar_t* mvm_wide_concat(const wchar_t* a, const wchar_t* b);

/* GetLastError() の値を人間が読める UTF-8 文字列にする。
 * 数値だけでは「126」のように原因が分からないため、必ず説明も出す。
 * 失敗時も必ず非 NULL を返す (数値のみの文字列)。 */
char* mvm_win_error_message(unsigned long error_code);

void mvm_str_free(void* p);

/*
 * コマンドライン引数を UTF-8 で取得する。
 *
 * なぜ必要か (V10 の実測所見):
 *   Windows の main(int argc, char** argv) が受け取る argv は UTF-8 ではなく
 *   プロセスの ANSI コードページ (日本語環境では CP932) でエンコードされている。
 *   UTF-8 前提で変換すると日本語パスで失敗し、さらに悪いことに、
 *   ANSI コードページで表現できない文字 (絵文字など) は復元不能に壊れる。
 *
 *   実際に S4 で、日本語ディレクトリを引数に渡した際、MLT 自身は *A API で
 *   処理できたのに mvm 側の UTF-8 変換だけが失敗するという食い違いが起きた。
 *
 *   よって引数は GetCommandLineW から取り直す。これが Windows で
 *   Unicode 引数を正しく受け取る唯一の方法である。
 *
 * 戻り値は mvm_win_free_utf8_args() で解放する。失敗時は NULL。
 */
char** mvm_win_get_utf8_args(int* out_argc);

void mvm_win_free_utf8_args(char** argv, int argc);

/* コンソール出力を UTF-8 にする。これをしないと、FormatMessageW 由来の
 * 日本語メッセージや日本語パスがコンソールで文字化けし、
 * 検証結果を読めなくなる。main の冒頭で 1 度呼ぶ。 */
void mvm_enable_utf8_console(void);

#ifdef __cplusplus
}
#endif

#endif /* MVM_WIN_UTF8_H */
