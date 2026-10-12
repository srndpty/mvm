/* P3-5A: 検出器 (ThreadSanitizer・AddressSanitizer) の下で mvm_mlt_math_group.c を Linux の
 * gcc で build するための、<windows.h> の代わり。この source が使う Interlocked 系だけを、
 * gcc の atomic 組み込み (検出器が追える) へ置き換える。製品の build では使わない。 */
#ifndef MVM_SANITIZER_WINDOWS_SHIM_H
#define MVM_SANITIZER_WINDOWS_SHIM_H

typedef long long LONG64;
typedef long LONG;

#define InterlockedIncrement64(target) __atomic_add_fetch((target), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement64(target) __atomic_sub_fetch((target), 1, __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd64(target, value)                                                    \
    __atomic_fetch_add((target), (value), __ATOMIC_SEQ_CST)
#define InterlockedExchange(target, value) __atomic_exchange_n((target), (value), __ATOMIC_SEQ_CST)
/* 比較して交換し、元の値を返す。この source は (target, 0, 0) の形 (= 読むだけ) でしか使わない。 */
#define InterlockedCompareExchange64(target, exchange, comparand)                                  \
    __atomic_load_n((target), __ATOMIC_SEQ_CST)
#define InterlockedCompareExchange(target, exchange, comparand)                                    \
    __atomic_load_n((target), __ATOMIC_SEQ_CST)

#endif
