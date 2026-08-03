# 共通の警告設定。Phase 0 のスパイクでも、型・ABI 起因の事故を早期に出したいので
# 警告は最初から強めに入れておく。-Werror は Phase 0 では課さない
# (依存ヘッダ由来の警告で作業が止まるのを避けるため)。

add_library(mvm_warnings INTERFACE)
add_library(mvm::warnings ALIAS mvm_warnings)

target_compile_options(mvm_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
)
