/*
 * lint の negative test 用の意図的な違反コード。
 *
 * scripts/lint.ps1 の producer service 検査が、
 * mlt_factory_producer への "avformat" 直接指定を検出することを確認する。
 * このファイルはビルド対象ではない (tests/CMakeLists.txt から参照しない)。
 *
 * 検査が「コメント中の文字列」ではなく呼び出しの形に反応することを
 * 確かめるため、コメントにも "avformat" を書いてある。
 * mlt_factory_producer という語もコメントに含めてある。
 */

#include <framework/mlt.h>

void bad(mlt_profile profile, const char* path) {
    /* これは検出されるべき */
    mlt_producer p = mlt_factory_producer(profile, "avformat", path);
    (void)p;
}

void also_bad(mlt_profile profile, const char* path) {
    /* 複数行にまたがる呼び出しも検出されるべき */
    mlt_producer p = mlt_factory_producer(profile, "avformat", path);
    (void)p;
}
