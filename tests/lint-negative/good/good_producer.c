/*
 * lint の対照群。これは検出されてはいけない。
 *
 * - producer は loader (NULL) を使っている
 * - consumer 側の "avformat" は正当なので対象外
 */

#include <framework/mlt.h>

void good(mlt_profile profile, const char* path, const char* out) {
    mlt_producer p = mlt_factory_producer(profile, NULL, path);
    mlt_consumer c = mlt_factory_consumer(profile, "avformat", out);
    (void)p;
    (void)c;
}
