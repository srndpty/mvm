# Phase 0 の検証ハーネスとテスト。
# 性能最適化はここでは行わない (S7 以降)。

find_package(PkgConfig REQUIRED)
find_package(Git REQUIRED)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)

# --- mvm_bench --------------------------------------------------------------
# Qt に依存しない C++20 CLI。MLT のヘッダは include しない。
add_executable(mvm_bench
    harness/mvm_bench.cpp
    harness/mvm_bench_compose.cpp
    harness/mvm_bench_proxy.cpp
)

# ScrubCoalescer の純粋単体テスト。MLT にも Qt にも依存しない。
add_executable(mvm_test_scrub_coalescer harness/test_scrub_coalescer.cpp)
target_include_directories(mvm_test_scrub_coalescer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/harness)
target_link_libraries(mvm_test_scrub_coalescer PRIVATE mvm::warnings)
add_test(NAME scrub_coalescer_unit COMMAND mvm_test_scrub_coalescer)

# 同じ契約を実際の mutex / condition_variable の下で検査する。
# sleep には依存しない (tests/harness/test_scrub_threaded.cpp 冒頭を参照)。
add_executable(mvm_test_scrub_threaded harness/test_scrub_threaded.cpp)
target_include_directories(mvm_test_scrub_threaded PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/harness)
target_link_libraries(mvm_test_scrub_threaded PRIVATE mvm::warnings)
add_test(NAME scrub_coalescer_threaded COMMAND mvm_test_scrub_threaded)

# proxy path resolver。preview と final で解決先が違うことを検査する。
add_executable(mvm_test_proxy_resolver harness/test_proxy_resolver.cpp)
target_include_directories(mvm_test_proxy_resolver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/harness)
target_link_libraries(mvm_test_proxy_resolver PRIVATE mvm::warnings)
add_test(NAME proxy_resolver_unit COMMAND mvm_test_proxy_resolver)
set_tests_properties(scrub_coalescer_threaded PROPERTIES TIMEOUT 120)

# F3-C3-A3-T1専用。production appにwindow-state診断を混ぜない。
add_executable(mvm_p2_window_state_controller harness/p2_window_state_controller.cpp)
target_link_libraries(mvm_p2_window_state_controller PRIVATE dwmapi user32 gdi32 mvm::warnings)
target_link_options(mvm_p2_window_state_controller PRIVATE -municode)

target_link_libraries(mvm_bench PRIVATE
    psapi
    mvm::mlt
    mvm::util
    PkgConfig::PNG
    mvm::warnings
)

# ffprobe は必ず UCRT64 版を使う。PATH 上の別 FFmpeg
# (C:\tools や winget 版) へフォールバックしない。
target_compile_definitions(mvm_bench PRIVATE
    ${MVM_MLT_PATH_DEFS}
    MVM_FFPROBE_EXE="${MVM_UCRT64_ROOT}/bin/ffprobe.exe"
)

# --- テスト -----------------------------------------------------------------

# F3-B0.6-R4: hidden oracle非依存のexact offline mapper。
add_executable(mvm_p2_offline_mapping harness/p2_offline_mapping_cli.cpp)
target_link_libraries(mvm_p2_offline_mapping PRIVATE mvm::core Qt6::Core mvm::warnings)

add_executable(mvm_test_presentation_opportunity_mapper
    gpu_preview/test_presentation_opportunity_mapper.cpp)
target_link_libraries(mvm_test_presentation_opportunity_mapper PRIVATE mvm::core mvm::warnings)
add_test(NAME p2_presentation_opportunity_mapper_pure
    COMMAND mvm_test_presentation_opportunity_mapper)
set_tests_properties(p2_presentation_opportunity_mapper_pure PROPERTIES
    ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}")

add_executable(mvm_test_native_present_hook_token
    gpu_preview/test_native_present_hook_token.cpp)
target_link_libraries(mvm_test_native_present_hook_token PRIVATE mvm::preview_qt mvm::warnings)
add_test(NAME p2_c0_native_present_hook_token
    COMMAND mvm_test_native_present_hook_token)
set_tests_properties(p2_c0_native_present_hook_token PROPERTIES
    ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}")

set(MVM_SMOKE_DIR "${CMAKE_SOURCE_DIR}/tests/assets/smoke")
set(MVM_SMOKE_MANIFEST "${MVM_SMOKE_DIR}/manifest.json")
set(MVM_JP_DIR "${MVM_SMOKE_DIR}/素材/日本語 テスト/第1回　微分積分＆演習")

# 素材が無いときに「原因不明の失敗」にせず、実行すべきコマンドを案内する。
set(MVM_MAKE_MEDIA_HINT "pwsh scripts/make-testmedia.ps1 -Mode Smoke")

function(mvm_add_test name)
    cmake_parse_arguments(T "" "" "COMMAND;PROPERTIES" ${ARGN})
    add_test(NAME ${name} COMMAND ${T_COMMAND})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}"
        ${T_PROPERTIES}
    )
endfunction()

# --- negative test は終了コードを厳密に照合する -----------------------------
#
# CTest の WILL_FAIL は使わない。**「0 以外なら合格」でしかない。**
# 使い方エラー (2) を期待しているつもりでも、
# ヒープ破壊 (0xC0000374)、DLL ロード失敗、素材の取り違えでも緑になる。
# 「失敗したこと」ではなく「意図した理由で失敗したこと」を検査する。
#
# このリポジトリに WILL_FAIL は 1 件も無い。増やさないこと。
find_program(MVM_PWSH pwsh)
if(NOT MVM_PWSH)
    message(FATAL_ERROR
        "pwsh が見つかりません。negative test の終了コード照合に必要です。")
endif()

foreach(_p2_r4_visible_case Good NegativeOriginalCallback NegativeSyntheticDelay
        NegativeExpectedClass NegativeDisplayed)
    string(TOLOWER "${_p2_r4_visible_case}" _p2_r4_visible_name)
    mvm_add_test(p2_r4_visible_input_${_p2_r4_visible_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-r4-visible-input-contract.ps1"
            -Case ${_p2_r4_visible_case}
            -Exe "$<TARGET_FILE:mvm_p2_offline_mapping>"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-r4-visible-${_p2_r4_visible_name}")
endforeach()

foreach(_p2_c3_a3_t0_case IN ITEMS Good ParentMissing ReadyZero DisplayMismatch DwmLost MultipleDwmPid LifecycleOrder)
    string(TOLOWER "${_p2_c3_a3_t0_case}" _p2_c3_a3_t0_name)
    mvm_add_test(p2_c3_a3_t0_contract_${_p2_c3_a3_t0_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a3-t0-contract.ps1"
            -Case "${_p2_c3_a3_t0_case}"
            -Analyzer "${CMAKE_SOURCE_DIR}/scripts/p2-c3-a3-t0-stall-localization.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a3-t0-${_p2_c3_a3_t0_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

foreach(_p2_c3_a3_t1_condition_case IN ITEMS GoodVisible GoodOccluded GoodDirty NegativeVisibility
        NegativeCloak NegativeCoverage NegativeUnexpected NegativeRectChange NegativeDirtyTicks NegativeDwmIdentity
        NegativeEtwLoss GoodTargetInvalidate GoodTargetRedrawNow NegativeUserInput NegativeTargetDamage NegativeDamageFailure)
    string(TOLOWER "${_p2_c3_a3_t1_condition_case}" _p2_c3_a3_t1_condition_name)
    mvm_add_test(p2_c3_a3_t1_condition_${_p2_c3_a3_t1_condition_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a3-t1-condition-contract.ps1"
            -Case "${_p2_c3_a3_t1_condition_case}"
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-c3-a3-t1-condition.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a3-t1-condition-${_p2_c3_a3_t1_condition_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

foreach(_p2_c3_a3_t1_summary_case IN ITEMS Visibility Dirty Rejected OrderConfound NegativeOrder)
    string(TOLOWER "${_p2_c3_a3_t1_summary_case}" _p2_c3_a3_t1_summary_name)
    mvm_add_test(p2_c3_a3_t1_summary_${_p2_c3_a3_t1_summary_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a3-t1-summary-contract.ps1"
            -Case "${_p2_c3_a3_t1_summary_case}"
            -Summarizer "${CMAKE_SOURCE_DIR}/scripts/summarize-p2-c3-a3-t1.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a3-t1-summary-${_p2_c3_a3_t1_summary_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

foreach(_p2_c3_a3_t2_case IN ITEMS Good NegativeMissingStage NegativePreflight)
    string(TOLOWER "${_p2_c3_a3_t2_case}" _p2_c3_a3_t2_name)
    mvm_add_test(p2_c3_a3_t2_update_chain_${_p2_c3_a3_t2_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a3-t2-update-chain-contract.ps1"
            -Case "${_p2_c3_a3_t2_case}"
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-c3-a3-t2-update-chain.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a3-t2-update-chain-${_p2_c3_a3_t2_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

foreach(_p2_c3_a3_t2_summary_case IN ITEMS TargetDoesNotWake NegativeControl InvalidPositive OrderConfound NegativeRuntime)
    string(TOLOWER "${_p2_c3_a3_t2_summary_case}" _p2_c3_a3_t2_summary_name)
    mvm_add_test(p2_c3_a3_t2_summary_${_p2_c3_a3_t2_summary_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a3-t2-summary-contract.ps1"
            -Case "${_p2_c3_a3_t2_summary_case}"
            -Summarizer "${CMAKE_SOURCE_DIR}/scripts/summarize-p2-c3-a3-t2.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a3-t2-summary-${_p2_c3_a3_t2_summary_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

mvm_add_test(p2_d5_2_formal_architecture_prew2
    COMMAND "${MVM_PWSH}" -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-d5-2-formal-architecture-contract.ps1"
        -Phase PreW2 -SourceRoot "${CMAKE_SOURCE_DIR}"
    PROPERTIES LABELS "gpu_preview")

# P2-D5-2-W2-A.1: observer start -> 新規sample確認 -> measurement arm の順序不変量。
mvm_add_test(p2_d5_2_formal_architecture_w2a1
    COMMAND "${MVM_PWSH}" -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-d5-2-formal-architecture-contract.ps1"
        -Phase W2A1 -SourceRoot "${CMAKE_SOURCE_DIR}"
    PROPERTIES LABELS "gpu_preview")

foreach(_p2_d5_2_formal_v2_case IN ITEMS Good GoodPhysicalSurplus GoodBoundaryCohortDivergence AuthorityInvalidPropagated NegativeRetiredReason NegativeRetiredOpportunityRegression NegativeRuntimeOverride NegativeUnknownOutcome NegativeVblankSequence NegativeVblankObserver NegativeBoundaryNotBracketed NegativeOutputChanged NegativeEtwLoss NegativeRingOverflow NegativePresentedFrameMismatch NegativeMultiPresentedOrdinal NegativeIdentityE1 NegativeIdentityE2 NegativeIdentityP1 NegativeIdentityP2 NegativeIdentityP3 NegativeIdentityP4 NegativeIdentityU1 NegativeIdentityS1 NegativeIdentityS2 NegativeIdentityN1 NegativeContractVersion GoodSourceHalfRate GoodForeignIntentAtBoundary NegativeIntentMissing NegativeIntentAmbiguous NegativeIntentOutOfDomain NegativeIntentDuplicateDisplay)
    string(TOLOWER "${_p2_d5_2_formal_v2_case}" _p2_d5_2_formal_v2_name)
    mvm_add_test(p2_d5_2_formal_v2_${_p2_d5_2_formal_v2_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-d5-2-formal-v2-contract.ps1"
            -Case "${_p2_d5_2_formal_v2_case}"
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-d5-2-formal-v2.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-d5-2-formal-v2-${_p2_d5_2_formal_v2_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

# P2-D5-2-W2-A: physical VBlank domain shadowのexecutable contract。
# shadow only であること、boundaryがhalf-openで閉じること、required_intent_countと
# physical countの差をverdictにしないことを固定する。
foreach(_p2_d5_2_w2a_case IN ITEMS Good GoodSuccessorAfterEnd GoodEmptyDomain GoodPhysicalOverhang GoodPhysicalSurplus AuthorityInvalidStall AuthorityInvalidWindow NegativeNotShadowOnly NegativeCounterAuthorityChanged NegativePerformanceConnected NegativeWindowAuthority NegativeOpportunityAuthority NegativeDomainRelation NegativePerformanceField NegativeRetiredReason NegativeUnknownReason NegativeCanonicalMapping NegativeValidWithError NegativeInvalidWithoutError NegativeSequenceStatus NegativeLongInterval NegativeShortInterval NegativeRingOverflow NegativeWaitFailure NegativeCumulative NegativeOutputChanged NegativeBoundaryNotBracketed NegativePredecessorInDomain NegativeSuccessorBeforeEnd NegativeIdentityD1 NegativeIdentityD2 NegativeIdentityD3 NegativeIdentityD4 NegativeEmptyDomainOrigin NegativeEmptyDomainBracket NegativeIdentityX1 NegativeIdentityX2 NegativeIdentityX3 AuthorityInvalidPrerollTimeout NegativePrerollNotCompleted NegativePrerollTimeoutFlag NegativePrerollNotBeforeStart NegativePrerollAtStart GoodPrerollOrdinalNonZero)
    string(TOLOWER "${_p2_d5_2_w2a_case}" _p2_d5_2_w2a_name)
    mvm_add_test(p2_d5_2_w2a_physical_domain_${_p2_d5_2_w2a_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-d5-2-w2a-physical-domain-contract.ps1"
            -Case "${_p2_d5_2_w2a_case}"
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-d5-2-w2a-physical-domain.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-d5-2-w2a-${_p2_d5_2_w2a_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

# P2-D5-2-W2-B1: scheduler intent identityのABI v4 exact-copy transport。
foreach(_p2_d5_2_w2b1_case IN ITEMS GoodFormal GoodNonFormal NegativeAppV3QtV4 NegativeAppV4QtV3 NegativeTokenLayout NegativeRecordLayout NegativeLayoutSignature NegativeFormalInvalid NegativeOrdinalMutation NegativeValidityMutation NegativeDuplicateEmbeddedTokenSerial NegativeDuplicateNativeSerial NegativeNonFormalFabricated NegativeRingOverflow NegativeMissingToken NegativeTokenSetFailure NegativeAuthorityFailure NegativeHookUnavailable NegativeLayoutHandshake NegativeSecondProducer NegativeRequiredFormalModeFalse)
    string(TOLOWER "${_p2_d5_2_w2b1_case}" _p2_d5_2_w2b1_name)
    mvm_add_test(p2_d5_2_w2b1_intent_transport_${_p2_d5_2_w2b1_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-d5-2-w2-b1-intent-transport-contract.ps1"
            -Case "${_p2_d5_2_w2b1_case}"
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-d5-2-w2-b1-intent-transport.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
            -Output "${CMAKE_CURRENT_BINARY_DIR}/p2-d5-2-w2b1-${_p2_d5_2_w2b1_name}.json"
        PROPERTIES LABELS "gpu_preview")
endforeach()

# P2-D5-2-W2-B2: B1 native intentからPresentEvent terminal outcomeまでのshadow exact closure。
# Layer 2 cohortはnative submissionで固定し、DisplayedQPCのphysical mappingと性能集計は接続しない。
foreach(_p2_d5_2_w2b2_case IN ITEMS GoodPresented GoodDiscarded GoodDisplayedOutsideMeasurementDomain NegativeMissingPresentEvent NegativeDuplicatePresentEvent NegativeAmbiguousCandidate NegativeSequenceDiscontinuity NegativeThreadMismatch NegativeSyncIntervalMismatch NegativePresentFlagsMismatch NegativePresentStartOutsideNativeInterval NegativeSwapchainMismatch NegativeIntentJoinMutation NegativeFinalStateUnknown NegativeTerminalMissing NegativePresentedWithoutDisplayedQpc NegativeEtwLost NegativeEtwBufferLoss NegativeNativePresentCountMismatch NegativeNativePresentDuplicate)
    string(TOLOWER "${_p2_d5_2_w2b2_case}" _p2_d5_2_w2b2_name)
    mvm_add_test(p2_d5_2_w2b2_terminal_shadow_${_p2_d5_2_w2b2_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-d5-2-w2-b2-terminal-shadow-contract.ps1"
            -Case "${_p2_d5_2_w2b2_case}"
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-d5-2-w2-b2-terminal-shadow.ps1"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-d5-2-w2b2-${_p2_d5_2_w2b2_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

# P2-D5-2-W2-C0: Layer 2 cohort外を含むdisplay candidate coverage inventory。
foreach(_p2_d5_2_w2c0_case IN ITEMS GoodForeignBoundary NegativeMissingNative NegativeMissingIntent NegativePhysicalAuthority)
    string(TOLOWER "${_p2_d5_2_w2c0_case}" _p2_d5_2_w2c0_name)
    mvm_add_test(p2_d5_2_w2c0_display_candidate_inventory_${_p2_d5_2_w2c0_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-d5-2-w2-c0-display-candidate-inventory-contract.ps1"
            -Case "${_p2_d5_2_w2c0_case}"
            -Inventory "${CMAKE_SOURCE_DIR}/scripts/inventory-p2-d5-2-w2-c0-display-candidates.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-d5-2-w2c0-${_p2_d5_2_w2c0_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

# P2-D5-2-W2-C0.1: measurement窓を変えずにnative/physical capture envelopeを閉じる。
foreach(_p2_d5_2_w2c01_case IN ITEMS Good NegativeStartAfterArm NegativeEndMutation NegativeSuccessorBeforeEnd NegativeCloseBeforeSuccessor NegativeSuccessorTimeout NegativeWindowExtended)
    string(TOLOWER "${_p2_d5_2_w2c01_case}" _p2_d5_2_w2c01_name)
    mvm_add_test(p2_d5_2_w2c01_capture_envelope_${_p2_d5_2_w2c01_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-d5-2-w2-c01-capture-envelope-contract.ps1"
            -Case "${_p2_d5_2_w2c01_case}"
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-d5-2-w2-c01-capture-envelope.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-d5-2-w2c01-${_p2_d5_2_w2c01_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()
mvm_add_test(p2_d5_2_w2c01_capture_envelope_architecture
    COMMAND "${MVM_PWSH}" -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-d5-2-w2-c01-capture-envelope-architecture.ps1"
        -SourceRoot "${CMAKE_SOURCE_DIR}"
    PROPERTIES LABELS "gpu_preview")

foreach(_p2_c3_a3_t2_d1b2e_case IN ITEMS NotEvaluableAbiMismatch EvaluableSameAbi AbiMismatchWithHistoricalQt NegativeBadProofSchema)
    string(TOLOWER "${_p2_c3_a3_t2_d1b2e_case}" _p2_c3_a3_t2_d1b2e_name)
    mvm_add_test(p2_c3_a3_t2_d1b2_evaluability_${_p2_c3_a3_t2_d1b2e_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a3-t2-d1b2-evaluability-contract.ps1"
            -Case "${_p2_c3_a3_t2_d1b2e_case}"
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-c3-a3-t2-d1b2-rebuild-evaluability.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a3-t2-d1b2e-${_p2_c3_a3_t2_d1b2e_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

foreach(_p2_c3_a3_t2_d1b2_case IN ITEMS ExactAvailable ExeMissing QtGuiMissing UnchangedFromCurrent RebuildIsNotExact NegativeMissingIdentities)
    string(TOLOWER "${_p2_c3_a3_t2_d1b2_case}" _p2_c3_a3_t2_d1b2_name)
    mvm_add_test(p2_c3_a3_t2_d1b2_reconstruction_${_p2_c3_a3_t2_d1b2_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a3-t2-d1b2-reconstruction-contract.ps1"
            -Case "${_p2_c3_a3_t2_d1b2_case}"
            -Inventory "${CMAKE_SOURCE_DIR}/scripts/inventory-p2-c3-a3-t2-d1b2-runtime-reconstruction.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a3-t2-d1b2-${_p2_c3_a3_t2_d1b2_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

foreach(_p2_c3_a3_t2_d1b3a_case IN ITEMS OverlapReproducesBad IndependentLossNotSufficient OverlapNoPathChange StickyAfterRemove RecoveredAfterRemove NegativeUserInput NegativeEtwLoss)
    string(TOLOWER "${_p2_c3_a3_t2_d1b3a_case}" _p2_c3_a3_t2_d1b3a_name)
    mvm_add_test(p2_c3_a3_t2_d1b3a_overlap_${_p2_c3_a3_t2_d1b3a_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a3-t2-d1b3a-overlap-contract.ps1"
            -Case "${_p2_c3_a3_t2_d1b3a_case}"
            -Summarizer "${CMAKE_SOURCE_DIR}/scripts/summarize-p2-c3-a3-t2-d1b3a.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a3-t2-d1b3a-${_p2_c3_a3_t2_d1b3a_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

foreach(_p2_c3_a3_t2_d1b0_case IN ITEMS Good NegativeNotCaptured NegativeClaimsAuthority NegativeMissingSwapchainField NegativeCapabilityContaminated NegativeZeroIdentity)
    string(TOLOWER "${_p2_c3_a3_t2_d1b0_case}" _p2_c3_a3_t2_d1b0_name)
    mvm_add_test(p2_c3_a3_t2_d1b0_preflight_${_p2_c3_a3_t2_d1b0_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a3-t2-d1b0-preflight-contract.ps1"
            -Case "${_p2_c3_a3_t2_d1b0_case}"
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-c3-a3-t2-d1b0-preflight.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a3-t2-d1b0-${_p2_c3_a3_t2_d1b0_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

foreach(_p2_c3_a3_t2_d1b1_case IN ITEMS ReproducesBadRegime BothArmsGoodIndependent ComposedBothArms OrderTimeConfound StaticConfigDiffers NegativeUserInput NegativeEtwLoss)
    string(TOLOWER "${_p2_c3_a3_t2_d1b1_case}" _p2_c3_a3_t2_d1b1_name)
    mvm_add_test(p2_c3_a3_t2_d1b1_probe_${_p2_c3_a3_t2_d1b1_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a3-t2-d1b1-probe-contract.ps1"
            -Case "${_p2_c3_a3_t2_d1b1_case}"
            -Summarizer "${CMAKE_SOURCE_DIR}/scripts/summarize-p2-c3-a3-t2-d1b1.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a3-t2-d1b1-${_p2_c3_a3_t2_d1b1_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

foreach(_p2_c3_a3_t2_d1a_case IN ITEMS ComposedNotFailure BadWithoutGoodComposed NoBadRegime ParentFieldUnavailable ModeClassBoundary NegativeMissingPresentMode)
    string(TOLOWER "${_p2_c3_a3_t2_d1a_case}" _p2_c3_a3_t2_d1a_name)
    mvm_add_test(p2_c3_a3_t2_d1a_regime_${_p2_c3_a3_t2_d1a_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a3-t2-d1a-regime-contract.ps1"
            -Case "${_p2_c3_a3_t2_d1a_case}"
            -Inventory "${CMAKE_SOURCE_DIR}/scripts/inventory-p2-c3-a3-t2-d1a-regime.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a3-t2-d1a-${_p2_c3_a3_t2_d1a_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

foreach(_p2_c3_a3_t2_d0_case IN ITEMS IndependentOfDwm ExternalTransition UnknownPresentPath LegacySchemaUnresolved InsufficientGroups NegativeEtwLoss)
    string(TOLOWER "${_p2_c3_a3_t2_d0_case}" _p2_c3_a3_t2_d0_name)
    mvm_add_test(p2_c3_a3_t2_d0_presentation_path_${_p2_c3_a3_t2_d0_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a3-t2-d0-presentation-path-contract.ps1"
            -Case "${_p2_c3_a3_t2_d0_case}"
            -Analyzer "${CMAKE_SOURCE_DIR}/scripts/analyze-p2-c3-a3-t2-d0-presentation-path.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a3-t2-d0-${_p2_c3_a3_t2_d0_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

foreach(_p2_c3_a3_t2_c_case IN ITEMS C1InvalidationWakes C1ForcedRedrawRequired C1RegimeInvalid C1OrderConfound C2PaintProcessingWakes C2RedirectionSuspect C2RegimeInvalid NegativeUpdateChainVolume NegativeRuntime NegativeUserInput NegativeDamageCadence)
    string(TOLOWER "${_p2_c3_a3_t2_c_case}" _p2_c3_a3_t2_c_name)
    mvm_add_test(p2_c3_a3_t2_c_summary_${_p2_c3_a3_t2_c_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a3-t2-c-summary-contract.ps1"
            -Case "${_p2_c3_a3_t2_c_case}"
            -Summarizer "${CMAKE_SOURCE_DIR}/scripts/summarize-p2-c3-a3-t2-c.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a3-t2-c-summary-${_p2_c3_a3_t2_c_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

foreach(_p2_c3_a3_t2_startup_case IN ITEMS Good NegativeFlagGatedContext NegativeMissingNullGuard NegativeVisibleAtLoad NegativeShowBeforeAttach)
    string(TOLOWER "${_p2_c3_a3_t2_startup_case}" _p2_c3_a3_t2_startup_name)
    mvm_add_test(p2_c3_a3_t2_startup_order_${_p2_c3_a3_t2_startup_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a3-t2-startup-order-contract.ps1"
            -Case "${_p2_c3_a3_t2_startup_case}"
            -SourceRoot "${CMAKE_SOURCE_DIR}"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a3-t2-startup-order-${_p2_c3_a3_t2_startup_name}"
        PROPERTIES LABELS "gpu_preview")
endforeach()

foreach(_p2_c3_a2_lifecycle_case IN ITEMS Good MissingWaiting Order ParentMismatch Lost EarlierIncomplete)
    string(TOLOWER "${_p2_c3_a2_lifecycle_case}" _p2_c3_a2_lifecycle_name)
    add_test(
        NAME p2_c3_a2_lifecycle_contract_${_p2_c3_a2_lifecycle_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a2-lifecycle-contract.ps1"
            -Case "${_p2_c3_a2_lifecycle_case}"
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-c3-a2-lifecycle.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a2-lifecycle-${_p2_c3_a2_lifecycle_name}")
    set_tests_properties(p2_c3_a2_lifecycle_contract_${_p2_c3_a2_lifecycle_name} PROPERTIES LABELS "gpu_preview")
endforeach()

foreach(_p2_b1_shadow_case Good NegativeEarlyCommit NegativeFirstSolution
        NegativeFormalAuthority NegativePhysicalDoubleCount NegativeSourceAccounting
        NegativeCandidateWindow ExpectedNoSolutionFailure)
    string(TOLOWER "${_p2_b1_shadow_case}" _p2_b1_shadow_name)
    mvm_add_test(p2_b1_shadow_${_p2_b1_shadow_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-b1-shadow-contract.ps1"
            -Case ${_p2_b1_shadow_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-b1-shadow.ps1"
            -FailureChecker "${CMAKE_SOURCE_DIR}/scripts/check-p2-b1-shadow-failure.ps1"
            -Output "${CMAKE_CURRENT_BINARY_DIR}/p2-b1-shadow-${_p2_b1_shadow_name}.json")
endforeach()

foreach(_p2_c0_native_case Good NegativeMissingToken NegativeDuplicateSerial
        NegativeTokenMismatch NegativePresentFlags)
    string(TOLOWER "${_p2_c0_native_case}" _p2_c0_native_name)
    mvm_add_test(p2_c0_native_hook_${_p2_c0_native_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c0-native-hook-contract.ps1"
            -Case ${_p2_c0_native_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-c0-native-hook.ps1"
            -Output "${CMAKE_CURRENT_BINARY_DIR}/p2-c0-native-${_p2_c0_native_name}.json")
endforeach()

foreach(_p2_c0_etw_case Good GoodDiscarded GoodBoundaryStraddle NegativeUnknown NegativeLost
        NegativeOrder NegativeOutsideInterval NegativeCount)
    string(TOLOWER "${_p2_c0_etw_case}" _p2_c0_etw_name)
    mvm_add_test(p2_c0_native_etw_${_p2_c0_etw_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c0-native-etw-contract.ps1"
            -Case ${_p2_c0_etw_case}
            -Fixture "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c0-native-hook-contract.ps1"
            -NativeChecker "${CMAKE_SOURCE_DIR}/scripts/check-p2-c0-native-hook.ps1"
            -EtwChecker "${CMAKE_SOURCE_DIR}/scripts/check-p2-c0-native-etw.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c0-etw-${_p2_c0_etw_name}")
endforeach()

foreach(_p2_c1_case Good NegativeUnknown NegativeLost NegativeFrameRange
        NegativeOpportunityOrder NegativeDisplayedPayload)
    string(TOLOWER "${_p2_c1_case}" _p2_c1_name)
    mvm_add_test(p2_c1_display_authority_${_p2_c1_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c1-display-authority-contract.ps1"
            -Case ${_p2_c1_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-c1-display-authority.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c1-display-${_p2_c1_name}")
endforeach()

foreach(_p2_c2_case Good NegativeUnknown NegativeIdentity NegativePayload NegativeOrder)
    string(TOLOWER "${_p2_c2_case}" _p2_c2_name)
    mvm_add_test(p2_c2_display_discard_attribution_${_p2_c2_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c2-display-discard-attribution-contract.ps1"
            -Case ${_p2_c2_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-c2-display-discard-attribution.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c2-attribution-${_p2_c2_name}")
endforeach()

foreach(_p2_c2_reason_case Good NegativeZeroDiscard NegativeUnknownReason NegativeMissingReason
        NegativePresentedReason NegativeMissingDiagnosticField)
    string(TOLOWER "${_p2_c2_reason_case}" _p2_c2_reason_name)
    mvm_add_test(p2_c2_discard_reason_${_p2_c2_reason_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c2-discard-reason-contract.ps1"
            -Case ${_p2_c2_reason_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-c2-discard-reasons.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c2-reason-${_p2_c2_reason_name}")
endforeach()

foreach(_p2_c3_case GoodControl GoodDwmFlush GoodFrameLatency1 NegativeMissingBatch
        NegativeWrongLatency NegativeFlushCount)
    string(TOLOWER "${_p2_c3_case}" _p2_c3_name)
    mvm_add_test(p2_c3_submission_backpressure_${_p2_c3_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-submission-backpressure-contract.ps1"
            -Case ${_p2_c3_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-c3-submission-backpressure.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-backpressure-${_p2_c3_name}")
endforeach()

foreach(_p2_c3_a2_case Good NegativeModeMissing NegativeBatchZero NegativeLost NegativeOrder)
    string(TOLOWER "${_p2_c3_a2_case}" _p2_c3_a2_name)
    mvm_add_test(p2_c3_a2_offline_${_p2_c3_a2_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-c3-a2-offline-contract.ps1"
            -Case ${_p2_c3_a2_case}
            -Analyzer "${CMAKE_SOURCE_DIR}/scripts/p2-c3-a2-offline-attribution.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-c3-a2-${_p2_c3_a2_name}")
endforeach()

# ARGS に ';' を含む引数 (--map や --require-proxy-ids) を渡す場合は
# **'@SEMI@' と書く。** ここで ';' へ戻す。
# CMake の引数はリストなので、'\;' でエスケープしても関数へ渡す時点で
# 実体の ';' に戻り、そこで分割されてしまう。
# 実際に --map の 2 件目が落ち、「2 件必須」のテストが 1 件で通っていた。
function(mvm_add_exit_test name)
    cmake_parse_arguments(T "" "EXIT" "ARGS;PROPERTIES" ${ARGN})
    if(NOT DEFINED T_EXIT)
        message(FATAL_ERROR "mvm_add_exit_test(${name}) に EXIT がありません")
    endif()
    list(JOIN T_ARGS "|" _child_args)
    string(REPLACE "@SEMI@" ";" _child_args "${_child_args}")
    add_test(NAME ${name}
        COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/expect-exit.ps1"
                -ExpectExit ${T_EXIT} -Exe "$<TARGET_FILE:mvm_bench>"
                -ChildArgs "${_child_args}")
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}"
        ${T_PROPERTIES}
    )
endfunction()

# --- doctor -----------------------------------------------------------------

mvm_add_test(doctor COMMAND mvm_bench doctor)

# negative: 存在しない module dir を成功扱いしない。
# doctor の検査失敗は exit 3 (mismatch)。DLL ロード失敗 (1) と区別する。
mvm_add_exit_test(doctor_missing_module_dir EXIT 3
    ARGS doctor --module-dir "${CMAKE_CURRENT_BINARY_DIR}/no-such-module-dir")

# negative: 空の module dir (MLT が最も静かに縮退するケース) を成功扱いしない
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/empty-modules")
mvm_add_exit_test(doctor_empty_module_dir EXIT 3
    ARGS doctor --module-dir "${CMAKE_CURRENT_BINARY_DIR}/empty-modules")

# negative: data dir が空だと profile が既定値へ黙って落ちる。これを検出する。
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/empty-data")
mvm_add_exit_test(doctor_empty_data_dir EXIT 3
    ARGS doctor --data-dir "${CMAKE_CURRENT_BINARY_DIR}/empty-data")

# negative: 存在しない profile 名を成功扱いしない
mvm_add_exit_test(doctor_missing_profile EXIT 3
    ARGS doctor --profile mvm_no_such_profile)

# negative: 期待値が誤っていれば失敗する (profile 値の照合が効いていること)
mvm_add_exit_test(doctor_wrong_profile_expectation EXIT 3
    ARGS doctor --expect-width 1280 --expect-height 720)

# negative: SAR の期待値が誤っていれば失敗する。
# SAR を取り違えると V12 で「なぜか横に伸びる」形で表面化するため、
# doctor の段階で照合しておく。
mvm_add_exit_test(doctor_wrong_sar_expectation EXIT 3
    ARGS doctor --expect-sar-num 4 --expect-sar-den 3)

# SAR は gcd で正規化して比較する。2/2 は 1/1 と等価なので通るべき。
mvm_add_test(doctor_sar_normalized
    COMMAND mvm_bench doctor --expect-sar-num 2 --expect-sar-den 2)

# --- 素材の検証 -------------------------------------------------------------

mvm_add_test(verify_media_smoke
    COMMAND mvm_bench verify-media "${MVM_SMOKE_MANIFEST}"
    PROPERTIES
        REQUIRED_FILES "${MVM_SMOKE_MANIFEST}"
        FAIL_REGULAR_EXPRESSION "FAIL"
)

# --- manifest の negative test ---------------------------------------------
# 壊した manifest が失敗することを確認する。通ってしまうと verify-media が
# 「検証したつもり」で成功していることになり、V2 の判定根拠が無意味になる。
# 詳細は tests/fixtures/README.md。

set(MVM_FIXTURES "${CMAKE_SOURCE_DIR}/tests/fixtures")
set(MVM_S5_SCENARIO "${CMAKE_SOURCE_DIR}/bench/scenarios/s5-five-track.json")

# 対照群: 正しい最小 manifest は通る。
# これが落ちるなら、下の negative test は意図した箇所で落ちていない。
mvm_add_test(verify_media_fixture_good
    COMMAND mvm_bench verify-media "${MVM_FIXTURES}/good-minimal.json"
    PROPERTIES
        REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
        FAIL_REGULAR_EXPRESSION "FAIL")

# 期待する終了コードまで書く。manifest 自体が読めない (1) のか、
# 読めたうえで期待値と食い違った (3) のかは意味が違う。
# 「どちらでもよい」にすると、fixture を取り違えても緑になる。
set(MVM_BAD_MANIFESTS
    missing-schema      1   # schema_version が無い          -> manifest エラー
    schema-version      1   # schema_version が対応外        -> manifest エラー
    empty-assets        1   # assets が空                    -> manifest エラー
    unknown-kind        3   # kind が未知
    duplicate-id        3   # id が重複
    missing-expected    3   # expected が無い
    missing-field       3   # expected に必須フィールドが無い
    wrong-pix-fmt       3   # pix_fmt を改変
    wrong-sar           3   # SAR を改変
    wrong-duration      3   # duration を改変
    wrong-alpha         3   # alpha の期待範囲を改変
)
list(LENGTH MVM_BAD_MANIFESTS _bad_len)
math(EXPR _bad_last "${_bad_len} / 2 - 1")
foreach(i RANGE ${_bad_last})
    math(EXPR _bn "${i} * 2")
    math(EXPR _bc "${i} * 2 + 1")
    list(GET MVM_BAD_MANIFESTS ${_bn} bad)
    list(GET MVM_BAD_MANIFESTS ${_bc} bad_exit)
    string(REPLACE "-" "_" bad_name "${bad}")
    mvm_add_exit_test(verify_media_bad_${bad_name} EXIT ${bad_exit}
        ARGS verify-media "${MVM_FIXTURES}/bad-${bad}.json"
        PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4")
endforeach()

mvm_add_test(p2_formal_output_path_contract
    COMMAND "${MVM_PWSH}" -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-formal-output-path.ps1"
        -Guard "${CMAKE_SOURCE_DIR}/scripts/formal-output-path.ps1"
        -RepositoryRoot "${CMAKE_SOURCE_DIR}"
        -GitExe "${GIT_EXECUTABLE}")

# --- 破損・退化素材の negative test -----------------------------------------
# これらが成功扱いされないことを確認する。
#
# subtitle_only は破損していない。コンテナは正当だが映像も音声も無い、
# 「読めるが使えない」入力であり、壊れたバイト列とは別の経路を通る。

set(MVM_CORRUPT_DIR "${MVM_SMOKE_DIR}/_corrupt")

# ファイルは存在する (ので「開けない」= 1 ではない)。
# 開いた結果が使い物にならないことを検出する exit 3 を期待する。
foreach(corrupt zero random truncated text subtitle_only)
    mvm_add_exit_test(probe_corrupt_${corrupt} EXIT 3
        ARGS probe "${MVM_CORRUPT_DIR}/${corrupt}.mp4"
        PROPERTIES REQUIRED_FILES "${MVM_CORRUPT_DIR}/${corrupt}.mp4")

    mvm_add_exit_test(decode_corrupt_${corrupt} EXIT 3
        ARGS decode "${MVM_CORRUPT_DIR}/${corrupt}.mp4" --frame 0
        PROPERTIES REQUIRED_FILES "${MVM_CORRUPT_DIR}/${corrupt}.mp4")
endforeach()

# --- probe (V2) -------------------------------------------------------------

mvm_add_test(probe_h264
    COMMAND mvm_bench probe "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4")

mvm_add_test(probe_hevc
    COMMAND mvm_bench probe "${MVM_SMOKE_DIR}/v1080p60_hevc.mp4"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_hevc.mp4")

mvm_add_test(probe_png_alpha
    COMMAND mvm_bench probe "${MVM_SMOKE_DIR}/png_alpha.png"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/png_alpha.png")

mvm_add_test(probe_wav
    COMMAND mvm_bench probe "${MVM_SMOKE_DIR}/wav_48k.wav"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/wav_48k.wav")

mvm_add_test(probe_4k_h264
    COMMAND mvm_bench probe "${MVM_SMOKE_DIR}/v4k60_h264.mp4"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v4k60_h264.mp4")

mvm_add_test(probe_4k_hevc10
    COMMAND mvm_bench probe "${MVM_SMOKE_DIR}/v4k60_hevc10.mp4"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v4k60_hevc10.mp4")

# --- 日本語パス (V10) -------------------------------------------------------

mvm_add_test(probe_jp_path_video
    COMMAND mvm_bench probe "${MVM_JP_DIR}/講義映像　第1回＆演習.mp4"
    PROPERTIES REQUIRED_FILES "${MVM_JP_DIR}/講義映像　第1回＆演習.mp4")

mvm_add_test(probe_jp_path_png
    COMMAND mvm_bench probe "${MVM_JP_DIR}/数式オーバーレイ＆図.png"
    PROPERTIES REQUIRED_FILES "${MVM_JP_DIR}/数式オーバーレイ＆図.png")

mvm_add_test(probe_jp_path_wav
    COMMAND mvm_bench probe "${MVM_JP_DIR}/ナレーション　音声.wav"
    PROPERTIES REQUIRED_FILES "${MVM_JP_DIR}/ナレーション　音声.wav")

# --- decode + フレーム固有マーカー ------------------------------------------
# 要求フレームと、素材に焼き込まれたマーカーが一致することを確認する。
# OCR は使わない (docs/research/test-media-format.md)。

foreach(frame 0 1 137 299)
    mvm_add_test(decode_marker_h264_${frame}
        COMMAND mvm_bench decode "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
                --frame ${frame} --expect-marker
                --output "${CMAKE_CURRENT_BINARY_DIR}/decode_h264_${frame}.png"
        PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4")
endforeach()

mvm_add_test(decode_marker_hevc
    COMMAND mvm_bench decode "${MVM_SMOKE_DIR}/v1080p60_hevc.mp4"
            --frame 137 --expect-marker
            --output "${CMAKE_CURRENT_BINARY_DIR}/decode_hevc_137.png"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_hevc.mp4")

# 4K HEVC 10bit。10bit -> RGBA の変換を経てもマーカーが読めることを確認する。
# probe だけでは「ピクセルが正しく出てくるか」は分からない。
mvm_add_test(decode_marker_4k_hevc10
    COMMAND mvm_bench decode "${MVM_SMOKE_DIR}/v4k60_hevc10.mp4"
            --frame 137 --expect-marker
            --output "${CMAKE_CURRENT_BINARY_DIR}/decode_4k_hevc10_137.png"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v4k60_hevc10.mp4")

mvm_add_test(decode_marker_4k_h264
    COMMAND mvm_bench decode "${MVM_SMOKE_DIR}/v4k60_h264.mp4"
            --frame 42 --expect-marker
            --output "${CMAKE_CURRENT_BINARY_DIR}/decode_4k_h264_42.png"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v4k60_h264.mp4")

mvm_add_test(decode_marker_jp_path
    COMMAND mvm_bench decode "${MVM_JP_DIR}/講義映像　第1回＆演習.mp4"
            --frame 200 --expect-marker
            --output "${CMAKE_CURRENT_BINARY_DIR}/decode_jp_200.png"
    PROPERTIES REQUIRED_FILES "${MVM_JP_DIR}/講義映像　第1回＆演習.mp4")

# negative: 範囲外フレームを成功扱いしない。
# ファイルは開けるので 3 (取得結果が期待と違う)。
mvm_add_exit_test(decode_out_of_range EXIT 3
    ARGS decode "${MVM_SMOKE_DIR}/v1080p60_h264.mp4" --frame 999999
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4")

# negative: 存在しない素材を成功扱いしない。
# こちらは開けないので 1。上の 3 と区別できることが要点である。
mvm_add_exit_test(probe_missing_file EXIT 1
    ARGS probe "${CMAKE_CURRENT_BINARY_DIR}/no-such-media.mp4")

# --- S5 音声 (M3) -----------------------------------------------------------
# 音声の正式なゲートは render-audio + verify-audio である。
# mvm_mlt_compose_audio (生バッファ) は fail-closed のままで、
# verify-compose の音声検査には使わない。

set(MVM_DIAG_DIR "${MVM_SMOKE_DIR}/_diag")
set(MVM_AUDIO_OUT "${CMAKE_CURRENT_BINARY_DIR}/audio")
file(MAKE_DIRECTORY "${MVM_AUDIO_OUT}")

# 最小グラフの切り分けケース。
# producer service に loader を使わないと AAC が壊れ volume が落ちることが
# 実測で分かっている (docs/research/composition-notes.md)。
# これらはその回帰テストである。
set(MVM_AG_CASES
    audio_direct_aac        A   # producer -> consumer のみ
    audio_playlist_aac      B   # + playlist + tractor
    audio_stream_selectors  C   # + video_index=-1
    audio_separate_resources D  # 別 resource
    audio_same_resource     E   # 同一 resource を別 producer で stream 分離
    audio_mix_sum           F   # mix(sum=1)
    audio_volume_minus_6db  G   # + volume -6dB
    audio_full_five_track   H   # 5 トラック相当
)
list(LENGTH MVM_AG_CASES _ag_len)
math(EXPR _ag_last "${_ag_len} / 2 - 1")
foreach(i RANGE ${_ag_last})
    math(EXPR _n "${i} * 2")
    math(EXPR _c "${i} * 2 + 1")
    list(GET MVM_AG_CASES ${_n} _name)
    list(GET MVM_AG_CASES ${_c} _case)
    mvm_add_test(${_name}
        COMMAND mvm_bench audio-graph-probe --case ${_case}
                --output "${MVM_AUDIO_OUT}/${_case}.wav"
                --diag-dir "${MVM_DIAG_DIR}" --smoke-dir "${MVM_SMOKE_DIR}"
                --timeout-ms 60000
        PROPERTIES REQUIRED_FILES "${MVM_DIAG_DIR}/a1_av.mp4")
endforeach()

# negative: 書き込めない出力先を成功扱いしない
mvm_add_exit_test(audio_invalid_output_negative EXIT 3
    ARGS audio-graph-probe --case A
         --output "${CMAKE_CURRENT_BINARY_DIR}/no-such-dir/x/y.wav"
         --diag-dir "${MVM_DIAG_DIR}" --smoke-dir "${MVM_SMOKE_DIR}"
    PROPERTIES REQUIRED_FILES "${MVM_DIAG_DIR}/a1_av.mp4")

# negative: timeout を成功扱いしない (1ms では絶対に終わらない)。
# timeout は専用の exit 4。書き込み失敗 (3) と取り違えないこと。
mvm_add_exit_test(audio_timeout_negative EXIT 4
    ARGS audio-graph-probe --case H
         --output "${MVM_AUDIO_OUT}/timeout.wav"
         --diag-dir "${MVM_DIAG_DIR}" --smoke-dir "${MVM_SMOKE_DIR}"
         --timeout-ms 1
    PROPERTIES REQUIRED_FILES "${MVM_DIAG_DIR}/a1_av.mp4")

# --- M3 の正式ゲート: 5 トラック構成の音声を実ファイルで検証 ----------------
# render で 4 種類を出力し、verify で 4 周波数 / gain / clipping を検査する。

mvm_add_test(render_audio_a1_only
    COMMAND mvm_bench render-audio "${MVM_S5_SCENARIO}"
            --output "${MVM_AUDIO_OUT}/A1-only.wav" --disable-track A2
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}" FIXTURES_SETUP mvm_audio)

mvm_add_test(render_audio_a2_only
    COMMAND mvm_bench render-audio "${MVM_S5_SCENARIO}"
            --output "${MVM_AUDIO_OUT}/A2-only.wav" --disable-track A1
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}" FIXTURES_SETUP mvm_audio)

mvm_add_test(render_audio_a2_only_0db
    COMMAND mvm_bench render-audio "${MVM_S5_SCENARIO}"
            --output "${MVM_AUDIO_OUT}/A2-only-0db.wav" --disable-track A1 --gain-db 0
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}" FIXTURES_SETUP mvm_audio)

mvm_add_test(render_audio_mixed
    COMMAND mvm_bench render-audio "${MVM_S5_SCENARIO}" --output "${MVM_AUDIO_OUT}/mixed.wav"
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}" FIXTURES_SETUP mvm_audio)

mvm_add_test(verify_audio_four_frequencies
    COMMAND mvm_bench verify-audio "${MVM_AUDIO_OUT}" --check-mix
    PROPERTIES FIXTURES_REQUIRED mvm_audio)

mvm_add_test(verify_audio_gain_minus_6db
    COMMAND mvm_bench verify-audio "${MVM_AUDIO_OUT}" --check-gain
    PROPERTIES FIXTURES_REQUIRED mvm_audio)

mvm_add_test(verify_audio_no_clipping
    COMMAND mvm_bench verify-audio "${MVM_AUDIO_OUT}" --check-clipping
    PROPERTIES FIXTURES_REQUIRED mvm_audio)

# --- 参照所有権の反復テスト -------------------------------------------------
# MLT の参照所有権を間違えると 1 回では露見せず、回数を重ねて
# handle 数や RSS の線形増加として現れる。
# 通常 CTest では 100 回、時間がかかるので performance ラベルは付けない
# (所有権は正しさの問題であり性能ではない)。

# ownership は「正しさ」だけを見る。
# Working Set の増減だけを理由に ownership 失敗としない。
# メモリは memory_soak_diagnostic (stability ラベル) へ分離した。
mvm_add_test(ownership_soak_100
    COMMAND mvm_bench soak "${MVM_S5_SCENARIO}" --iterations 100
            --wav-dir "${MVM_AUDIO_OUT}" --no-memory-check
    PROPERTIES
        REQUIRED_FILES "${MVM_S5_SCENARIO}"
        TIMEOUT 1800)

# メモリは長時間かかるうえ、まだ合否を決められる段階にない。
# 通常 CTest から外し、stability ラベルで明示的に実行する。
mvm_add_test(memory_soak_diagnostic
    COMMAND mvm_bench memory-probe "${MVM_S5_SCENARIO}" --case C
            --csv "${CMAKE_CURRENT_BINARY_DIR}/memory_case_c.csv"
            --wav-dir "${MVM_AUDIO_OUT}"
    PROPERTIES
        REQUIRED_FILES "${MVM_S5_SCENARIO}"
        LABELS stability
        TIMEOUT 1800)

# --- scrub 表示契約の回帰テスト (M6) ----------------------------------------
# 閾値ではなく契約を見る。counter invariant・表示 generation の単調性・
# 最終要求の表示・marker 一致・集計の自己整合が破れたら失敗する。
# 性能の判定は scripts/scrub-matrix.ps1 で行う (8 条件 x 3 回)。
foreach(iv 8333 16667)
    mvm_add_test(scrub_contract_linear_${iv}
        COMMAND mvm_bench scrub-bench "${MVM_S5_SCENARIO}"
                --requests 60 --pattern linear --submit-interval-us ${iv}
        PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}" TIMEOUT 300)
endforeach()

# coalescing が実際に働いていることを確認する。
# superseded_pending が 0 なら、latest-only の置換が一度も起きていない。
mvm_add_test(scrub_coalescing_occurs
    COMMAND mvm_bench scrub-bench "${MVM_S5_SCENARIO}"
            --requests 60 --pattern linear --submit-interval-us 0
            --check --require-coalescing --min-updates-per-sec 0 --max-p95-ms 100000
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}" TIMEOUT 300)

# negative: 音声を含むケースで --wav-dir が無いとき、
# 音声を黙って飛ばした測定を成功扱いしない。
#
# 旧 memory_probe_${mcase}_requires_wav_dir (WILL_FAIL 版) は削除した。
# 下の memory_probe_${mcase}_exit_code_is_usage と同じことを、
# より弱い条件 (0 以外なら何でも合格) で見ていただけの重複である。

# --- S7: preview / proxy -----------------------------------------------------

set(MVM_S7_SCENARIO "${CMAKE_SOURCE_DIR}/bench/scenarios/s7-five-track-60s.json")
set(MVM_BENCH_DIR   "${CMAKE_SOURCE_DIR}/tests/assets/benchmark")
set(MVM_PROXY_DIR   "${MVM_BENCH_DIR}/_proxy")
set(MVM_MAKE_BENCH_HINT "pwsh scripts/make-testmedia.ps1 -Mode Benchmark")

# 60 秒 scenario の schema と代表フレームの合成。
# 映像だけを見る。音声の正式ゲートは render-audio + verify-audio である。
mvm_add_test(preview_scenario_schema
    COMMAND mvm_bench compose "${MVM_S7_SCENARIO}" --frame 3599 --expect-marker
    PROPERTIES
        REQUIRED_FILES "${MVM_S7_SCENARIO}"
        TIMEOUT 300)

# preview は短時間で契約だけ見る。性能判定は performance ラベル側。
mvm_add_test(preview_contract_short
    COMMAND mvm_bench preview-bench "${MVM_S5_SCENARIO}"
            --consumer null --real-time -4 --measure-ms 3000 --warmup-ms 0
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}" TIMEOUT 300)

# negative: real_time=0 は mlt_frame_get_image を呼ばないので計測に使えない。
# 走らせてから「rendered が 0 件」で落とす (3) のではなく、
# 走らせる前に使い方エラー (2) で止める。
# 3 のままでは「使い方が誤っている」のか「実装が壊れている」のか区別できない。
mvm_add_exit_test(preview_invalid_real_time_exit2 EXIT 2
    ARGS preview-bench "${MVM_S5_SCENARIO}" --real-time 0 --measure-ms 1000
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}")

# negative: 存在しない consumer service を成功扱いしない。
mvm_add_exit_test(preview_missing_consumer_service EXIT 3
    ARGS preview-bench "${MVM_S5_SCENARIO}" --consumer mvm_no_such_consumer
         --real-time -1 --measure-ms 1000
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}")

# negative: 音声ケースの --wav-dir 欠落は「使い方エラー (2)」であること。
foreach(mcase D E)
    mvm_add_exit_test(memory_probe_${mcase}_exit_code_is_usage EXIT 2
        ARGS memory-probe "${MVM_S5_SCENARIO}" --case ${mcase} --iterations 1
        PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}")
endforeach()

# --- required proxy id 契約 (S7.1) ------------------------------------------
# 「全 source を proxy 必須」にはできない (WAV や音声専用 source が混ざる)。
# required と optional を分け、required が 1 件でも未解決なら exit 4 で落とす。
#
# **negative test には実在する proxy を使う。**
# 以前は `_proxy/a.mp4` のような架空のパスを使っていた。
# 架空のパスでは「required が解決できた」ことにならず、
# 文字列置換が成功しただけの状態を「合格」として見てしまう。
# resolver が proxy の実体を検査するようになった今、
# 架空のパスは常に exit 4 になり、正常系の証明にもならない。

set(MVM_S7_4K     "${CMAKE_SOURCE_DIR}/bench/scenarios/s7-4k-original.json")
set(MVM_PX_V1     "_proxy/v4k60_h264_proxy_gop12.mp4")
set(MVM_PX_V2     "_proxy/v1080p60_hevc_proxy_gop12.mp4")
set(MVM_PX_V1_ABS "${MVM_PROXY_DIR}/v4k60_h264_proxy_gop12.mp4")
set(MVM_PX_V2_ABS "${MVM_PROXY_DIR}/v1080p60_hevc_proxy_gop12.mp4")
# ';' は CMake のリスト区切りなので、1 引数として渡すためエスケープする。
set(MVM_REQ2 "v4k60_h264.mp4@SEMI@v1080p60_hevc.mp4")
set(MVM_MAP2 "v4k60_h264.mp4=${MVM_PX_V1}@SEMI@v1080p60_hevc.mp4=${MVM_PX_V2}")

# proxy が無い環境ではこの契約は検査できない。素材生成を促して skip する。
if(EXISTS "${MVM_PX_V1_ABS}" AND EXISTS "${MVM_PX_V2_ABS}")

    # 正常系: 実在する proxy 2 件が両方置換される。
    mvm_add_exit_test(proxy_required_two_real_proxies_resolved EXIT 0
        ARGS resolve-proxy "${MVM_S7_4K}" --target preview
             --map "${MVM_MAP2}" --require-proxy-ids "${MVM_REQ2}"
        PROPERTIES REQUIRED_FILES "${MVM_S7_4K}")

    # map にはあるが proxy の実体が無い -> exit 4。
    # 文字列置換が成功しただけを「解決できた」と読まないことの検査である。
    mvm_add_exit_test(proxy_required_missing_proxy_file_fails EXIT 4
        ARGS resolve-proxy "${MVM_S7_4K}" --target preview
             --map "v4k60_h264.mp4=${MVM_PX_V1}@SEMI@v1080p60_hevc.mp4=_proxy/mvm_no_such_proxy.mp4"
             --require-proxy-ids "${MVM_REQ2}"
        PROPERTIES REQUIRED_FILES "${MVM_S7_4K}")

    # required の片方だけ map にある -> exit 4
    mvm_add_exit_test(proxy_required_partial_map_fails EXIT 4
        ARGS resolve-proxy "${MVM_S7_4K}" --target preview
             --map "v4k60_h264.mp4=${MVM_PX_V1}"
             --require-proxy-ids "${MVM_REQ2}"
        PROPERTIES REQUIRED_FILES "${MVM_S7_4K}")

    # scenario に存在しない required id -> exit 4。
    # proxy 自体は実在するものを指す。落ちる理由を 1 つに絞るためである。
    mvm_add_exit_test(proxy_required_absent_in_scenario_fails EXIT 4
        ARGS resolve-proxy "${MVM_S7_4K}" --target preview
             --map "mvm_no_such_source.mp4=${MVM_PX_V1}"
             --require-proxy-ids "mvm_no_such_source.mp4"
        PROPERTIES REQUIRED_FILES "${MVM_S7_4K}")

    # 同じ required id を二度指定するのは使い方の誤り -> exit 2。
    # 黙って 1 件として扱うと「2 件必須のつもりが 1 件」に気づけない。
    mvm_add_exit_test(proxy_required_duplicate_id_is_usage_error EXIT 2
        ARGS resolve-proxy "${MVM_S7_4K}" --target preview
             --map "${MVM_MAP2}"
             --require-proxy-ids "v4k60_h264.mp4@SEMI@v4k60_h264.mp4"
        PROPERTIES REQUIRED_FILES "${MVM_S7_4K}")

    # final では required も original のまま。proxy になったら exit 3。
    mvm_add_exit_test(proxy_required_final_keeps_original EXIT 0
        ARGS resolve-proxy "${MVM_S7_4K}" --target final
             --map "${MVM_MAP2}" --require-proxy-ids "${MVM_REQ2}"
        PROPERTIES REQUIRED_FILES "${MVM_S7_4K}")

    # final は proxy に依存しない。
    # **同じ「proxy の実体が無い」状況で、preview は exit 4、final は exit 0** に
    # なることを対で確かめる。preview 側は proxy_required_missing_proxy_file_fails。
    # final は required id が scenario にあり全出現が original のままなら成功する。
    mvm_add_exit_test(proxy_final_succeeds_without_proxy_file EXIT 0
        ARGS resolve-proxy "${MVM_S7_4K}" --target final
             --map "v4k60_h264.mp4=${MVM_PX_V1}@SEMI@v1080p60_hevc.mp4=_proxy/mvm_no_such_proxy.mp4"
             --require-proxy-ids "${MVM_REQ2}"
        PROPERTIES REQUIRED_FILES "${MVM_S7_4K}")

    # map を一切与えなくても final は成功する。
    # final は preview proxy の map 登録を要求しない。
    mvm_add_exit_test(proxy_final_succeeds_without_map EXIT 0
        ARGS resolve-proxy "${MVM_S7_4K}" --target final
             --require-proxy-ids "${MVM_REQ2}"
        PROPERTIES REQUIRED_FILES "${MVM_S7_4K}")

    # out ファイルの契約。終了コードだけでは見えない部分を検査する。
    #   - 失敗時に out を新規作成しない
    #   - 失敗時に既存の out を変更しない
    #   - 一時ファイル (.mvmtmp) を残さない
    #   - final の出力に proxy パスが 1 件も無い
    #   - optional (音声専用 source / WAV) は original のまま
    add_test(NAME proxy_resolver_out_file_contract
        COMMAND "${MVM_PWSH}" -NoProfile -File
                "${CMAKE_SOURCE_DIR}/scripts/check-resolver-contract.ps1"
                -Bench "$<TARGET_FILE:mvm_bench>"
                -Scenario "${MVM_S7_4K}"
                -WorkDir "${CMAKE_CURRENT_BINARY_DIR}/resolver-contract")
    set_tests_properties(proxy_resolver_out_file_contract PROPERTIES
        ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}"
        REQUIRED_FILES "${MVM_S7_4K}"
        TIMEOUT 300)
else()
    message(STATUS
        "proxy が無いため required proxy id 契約のテストを登録しません。"
        " 先に ${MVM_MAKE_BENCH_HINT} と pwsh scripts/proxy-matrix.ps1 を実行してください。")
endif()

# 旧 --require-proxy は diagnostic 専用へ降格した。
# 「0 件なら失敗」であることは維持するが、判定には使わない。
mvm_add_exit_test(proxy_legacy_require_proxy_diagnostic EXIT 3
    ARGS resolve-proxy "${MVM_S5_SCENARIO}" --target preview
         --map "mvm_no_such_source.mp4=_proxy/x.mp4" --require-proxy
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}")

# negative: メタデータが違う proxy を成功扱いしない。
# 1080p 素材を 4K 素材の proxy と称して渡す。frame count も解像度も違う。
mvm_add_exit_test(proxy_mapping_metadata_mismatch_negative EXIT 3
    ARGS proxy-mapping
         --original "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
         --proxy "${MVM_SMOKE_DIR}/v4k60_h264.mp4"
         --random 0
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4")
set_tests_properties(proxy_mapping_metadata_mismatch_negative PROPERTIES
    REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4;${MVM_SMOKE_DIR}/v4k60_h264.mp4")

# --- performance ラベル ------------------------------------------------------
# 長時間の性能試験。通常 CTest には混ぜない。
# 実行: pwsh scripts/test.ps1 -Preset ucrt64-release -Performance
#
# **このラベルの緑を M5 / M8 の合格根拠として扱わない。**
# ここにあるのは「経路が壊れていないこと」を短時間で見る smoke であり、
# 閾値判定を含まない。M5 / M8 の判定は
#   pwsh scripts/seek-matrix.ps1     (M5: 1000 点 x 3 回)
#   pwsh scripts/preview-matrix.ps1  (M8: 60 秒 x 3 回)
# の記録された数値だけで行う。
#
# **テスト名は測っている対象と一致させる。**
# 旧名の seek_proxy / scrub_proxy は 1080p native の scenario を測っており
# proxy を一切使っていなかった。preview_4k_proxy は V1 だけを proxy 化した
# 部分 proxy を測っていた。名前で対象を誤認すると、判定そのものを誤る。

mvm_add_test(preview_1080p60_native_smoke
    COMMAND mvm_bench preview-bench "${MVM_S7_SCENARIO}"
            --consumer null --real-time -8 --measure-ms 30000 --warmup-ms 5000
    PROPERTIES
        REQUIRED_FILES "${MVM_S7_SCENARIO}"
        LABELS performance
        TIMEOUT 900)

# native (1080p) の seek。proxy は使っていない。
mvm_add_test(native_seek_smoke
    COMMAND mvm_bench seek-bench "${MVM_S7_SCENARIO}" --random 100
    PROPERTIES
        REQUIRED_FILES "${MVM_S7_SCENARIO}"
        LABELS performance
        TIMEOUT 1800)

# native (1080p) の scrub。proxy は使っていない。
mvm_add_test(native_scrub_smoke
    COMMAND mvm_bench scrub-bench "${MVM_S7_SCENARIO}"
            --requests 300 --pattern linear --submit-interval-us 16667
    PROPERTIES
        REQUIRED_FILES "${MVM_S7_SCENARIO}"
        LABELS performance
        TIMEOUT 1800)

if(EXISTS "${MVM_PX_V1_ABS}")
    mvm_add_test(proxy_generate_4k
        COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/proxy-matrix.ps1"
                -Candidates gop12 -Force
        PROPERTIES LABELS performance TIMEOUT 1800)

    # V1 だけを proxy 化した部分 proxy。**正式な M8 評価ではない。**
    mvm_add_test(preview_partial_proxy_diagnostic
        COMMAND mvm_bench preview-bench
                "${CMAKE_SOURCE_DIR}/bench/scenarios/s7-4k-proxy-gop12.json"
                --consumer null --real-time -8 --measure-ms 30000 --warmup-ms 5000
        PROPERTIES
            REQUIRED_FILES "${CMAKE_SOURCE_DIR}/bench/scenarios/s7-4k-proxy-gop12.json"
            LABELS performance
            TIMEOUT 900)

    mvm_add_test(proxy_frame_mapping
        COMMAND mvm_bench proxy-mapping
                --original "${MVM_BENCH_DIR}/v4k60_h264.mp4"
                --proxy "${MVM_PX_V1_ABS}"
                --random 200 --seed 20260804
        PROPERTIES LABELS performance TIMEOUT 1800)
endif()

# all-video proxy (V1 + V2)。正式な M8 / M5 が対象にしている構成である。
if(EXISTS "${MVM_PX_V1_ABS}" AND EXISTS "${MVM_PX_V2_ABS}")
    set(MVM_S7_4K_ALL "${CMAKE_SOURCE_DIR}/bench/scenarios/s7-4k-proxy-all-gop12.json")

    # **事前検査**: scenario が本当に V1 / V2 の両方を proxy 化しているか。
    # 部分 proxy の scenario を all-video のつもりで測るのを防ぐ。
    # required 2 件が解決できなければ exit 4 で落ちる。
    mvm_add_exit_test(preview_all_video_proxy_precheck EXIT 0
        ARGS resolve-proxy "${MVM_S7_4K}" --target preview
             --map "${MVM_MAP2}" --require-proxy-ids "${MVM_REQ2}"
        PROPERTIES
            REQUIRED_FILES "${MVM_S7_4K}"
            LABELS performance
            FIXTURES_SETUP mvm_all_proxy
            TIMEOUT 300)

    # 短時間の compose smoke。合成経路が all-video proxy で通ることを見る。
    mvm_add_test(compose_all_video_proxy_smoke
        COMMAND mvm_bench compose "${MVM_S7_4K_ALL}" --frame 3599 --expect-marker
        PROPERTIES
            REQUIRED_FILES "${MVM_S7_4K_ALL}"
            LABELS performance
            FIXTURES_REQUIRED mvm_all_proxy
            TIMEOUT 600)

    # 短時間の preview smoke。**判定用の 60 秒測定ではない。**
    mvm_add_test(preview_all_video_proxy_diagnostic
        COMMAND mvm_bench preview-bench "${MVM_S7_4K_ALL}"
                --consumer null --real-time -1 --measure-ms 10000 --warmup-ms 3000
        PROPERTIES
            REQUIRED_FILES "${MVM_S7_4K_ALL}"
            LABELS performance
            FIXTURES_REQUIRED mvm_all_proxy
            TIMEOUT 900)
endif()

# --- Phase 1 / P1: GPU preview -----------------------------------------------
# 通常 CTest で回すのは短時間の契約検査だけ。
# 60 秒 x 3 run の性能計測は scripts/p1-matrix.ps1 で行い、
# その結果だけを P1 の合否判定に使う (docs/phase1-plan.md §11 / §12)。

# --- Phase 5 / P5-B: PreviewEngine product foundation -----------------------
set(_mvm_p5d1_tests)

add_executable(mvm_test_checked_output_timebase core/test_checked_output_timebase.cpp)
target_link_libraries(mvm_test_checked_output_timebase PRIVATE mvm::core mvm::warnings)
mvm_add_test(preview_timebase_p5d1_unit COMMAND mvm_test_checked_output_timebase
    PROPERTIES LABELS p5d1)
list(APPEND _mvm_p5d1_tests preview_timebase_p5d1_unit)

add_executable(mvm_test_preview_engine preview_engine/test_preview_engine.cpp)
target_link_libraries(mvm_test_preview_engine PRIVATE mvm::preview_engine mvm::warnings)
mvm_add_test(preview_engine_p5b_unit COMMAND mvm_test_preview_engine
    PROPERTIES LABELS p5b)
set_property(TEST preview_engine_p5b_unit APPEND PROPERTY LABELS p5d1)
list(APPEND _mvm_p5d1_tests preview_engine_p5b_unit)

add_test(NAME preview_engine_p5b_unsafe_destructor
    COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/expect-exit.ps1"
            -ExpectExit 86 -Exe "$<TARGET_FILE:mvm_test_preview_engine>"
            -ChildArgs "|--unsafe-destructor")
set_tests_properties(preview_engine_p5b_unsafe_destructor PROPERTIES
    ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}"
    LABELS p5b)

add_executable(mvm_test_preview_public_boundary
    preview_engine/test_preview_public_boundary.cpp)
target_link_libraries(mvm_test_preview_public_boundary PRIVATE mvm::preview_engine mvm::warnings)
mvm_add_test(preview_engine_p5b_public_compile COMMAND mvm_test_preview_public_boundary
    PROPERTIES LABELS p5b)
mvm_add_test(preview_engine_p5b_public_tokens
    COMMAND "${MVM_PWSH}" -NoProfile
            -File "${CMAKE_CURRENT_SOURCE_DIR}/preview_engine/check-public-boundary.ps1"
            -PublicDir "${CMAKE_SOURCE_DIR}/src/preview_engine"
    PROPERTIES LABELS p5b)

add_executable(mvm_test_p5c_native_attach
    preview_engine/test_p5c_native_attach.cpp)
target_link_libraries(mvm_test_p5c_native_attach PRIVATE
    mvm::preview_engine d3d11 mvm::warnings)
mvm_add_test(preview_engine_p5c_native_attach COMMAND mvm_test_p5c_native_attach
    PROPERTIES LABELS p5c)
set_tests_properties(preview_engine_p5c_native_attach PROPERTIES
    LABELS "p5c;workstation")

set(_p5c_fixture_a "${CMAKE_SOURCE_DIR}/tests/assets/p3_audio/p3_av_h264_aac.mp4")
set(_p5e_fixture_b "${CMAKE_SOURCE_DIR}/tests/assets/p3_audio/p3_video_hevc_b.mp4")
set(_p5c_fixture_hint "pwsh scripts/make-p3-fixture.ps1")
set(_p5c_fixture_wrapper "${CMAKE_SOURCE_DIR}/scripts/run-with-required-fixture.ps1")

mvm_add_test(preview_engine_p5c_fixture_wrapper_contract
    COMMAND "${MVM_PWSH}" -NoProfile
            -File "${CMAKE_CURRENT_SOURCE_DIR}/preview_engine/check-required-fixture-wrapper.ps1"
            -Wrapper "${_p5c_fixture_wrapper}"
            -PresentFile "${CMAKE_SOURCE_DIR}/CMakeLists.txt"
            -MissingFile "${CMAKE_CURRENT_BINARY_DIR}/p5c-missing-fixture.mp4"
    PROPERTIES LABELS p5c)

if(MVM_ENABLE_QT)
    set(_mvm_p5c_product_tests)
    macro(mvm_add_p5c_product_test name)
        cmake_parse_arguments(T "" "" "ARGS" ${ARGN})
        set(_command
            "${MVM_PWSH}" -NoProfile -File "${_p5c_fixture_wrapper}"
            -RequiredFile "${_p5c_fixture_a}" -Hint "${_p5c_fixture_hint}"
            -Exe "$<TARGET_FILE:mvm_p5c_preview_smoke>")
        if(T_ARGS)
            list(JOIN T_ARGS "|" _child_args)
            list(APPEND _command -ChildArgs "${_child_args}")
        endif()
        mvm_add_test(${name} COMMAND ${_command}
            PROPERTIES LABELS p5c TIMEOUT 40)
        set_tests_properties(${name} PROPERTIES LABELS "p5c;workstation")
        list(APPEND _mvm_p5c_product_tests ${name})
    endmacro()

    mvm_add_p5c_product_test(preview_engine_p5c_product_smoke)
    mvm_add_p5c_product_test(preview_engine_p5c_gpu_drain_failure ARGS --fault-gpu-drain)
    mvm_add_p5c_product_test(preview_engine_p5c_device_fatal ARGS --fault-device)
    mvm_add_p5c_product_test(preview_engine_p5c_decoder_fatal ARGS --fault-decoder)
    mvm_add_p5c_product_test(preview_engine_p5c_shutdown_source_startup
        ARGS --shutdown-source-startup)
    mvm_add_p5c_product_test(preview_engine_p5c_pre_attach_shutdown
        ARGS --shutdown-before-attach)
    mvm_add_p5c_product_test(preview_engine_p5c_engine_lifetime_detach
        ARGS --engine-lifetime-detach)
    mvm_add_p5c_product_test(preview_engine_p5c_renderer_recreation
        ARGS --renderer-recreation)
    mvm_add_p5c_product_test(preview_engine_p5c_decoder_eof
        ARGS --decoder-eof)

    # wall-clockでrender idleを測るtestは、count採取時点でin-flightだったrender pass
    # が後から完了すると偽陽性になる。並列実行のCPU競合で実際に表面化した。
    # 検査は弱めず、これらだけ他と同時に走らせない。
    set_tests_properties(preview_engine_p5c_engine_lifetime_detach
                         preview_engine_p5c_decoder_eof
                         PROPERTIES RUN_SERIAL TRUE)

    list(LENGTH _mvm_p5c_product_tests _mvm_p5c_product_registered)
    if(NOT _mvm_p5c_product_registered EQUAL 9)
        message(FATAL_ERROR
            "[mvm] P5-C product testの登録数が不正です: ${_mvm_p5c_product_registered}/9")
    endif()

    # --- Phase 5 / P5-D2: audio-master transport ----------------------------
    # WASAPI endpoint と D3D11 device を要求するため workstation label を付ける。
    set(_mvm_p5d_product_tests)
    macro(mvm_add_p5d_product_test name)
        cmake_parse_arguments(T "" "" "ARGS" ${ARGN})
        set(_command
            "${MVM_PWSH}" -NoProfile -File "${_p5c_fixture_wrapper}"
            -RequiredFile "${_p5c_fixture_a}" -Hint "${_p5c_fixture_hint}"
            -Exe "$<TARGET_FILE:mvm_p5d_preview_smoke>")
        if(T_ARGS)
            list(JOIN T_ARGS "|" _child_args)
            list(APPEND _command -ChildArgs "${_child_args}")
        endif()
        mvm_add_test(${name} COMMAND ${_command}
            PROPERTIES LABELS p5d TIMEOUT 60)
        set_tests_properties(${name} PROPERTIES LABELS "p5d;workstation")
        list(APPEND _mvm_p5d_product_tests ${name})
    endmacro()

    mvm_add_p5d_product_test(preview_engine_p5d_audio_master_smoke)
    mvm_add_p5d_product_test(preview_engine_p5d_audio_clock_fail_closed
        ARGS --fault-audio-clock)
    mvm_add_p5d_product_test(preview_engine_p5d_audio_sink_runtime_fault
        ARGS --fault-audio-sink)
    mvm_add_p5d_product_test(preview_engine_p5d_pause_fail_closed ARGS --fault-audio-pause)
    # audio登録済みでGPU drainを失敗させ、quarantine (異常teardown) 経路の
    # terminal state / quarantine / shutdown sequence を固定する。
    # destruction order自体のregression testではない。
    mvm_add_p5d_product_test(preview_engine_p5d_gpu_drain_with_audio ARGS --fault-gpu-drain)
    # P5-D3: exact seek。requestの受理と completion を分けて固定する。
    mvm_add_p5d_product_test(preview_engine_p5d_exact_seek_paused ARGS --seek-paused)
    # decode readyだけでcompleteにしないことを検出する negative test。
    mvm_add_p5d_product_test(preview_engine_p5d_seek_presentation_fail_closed
        ARGS --fault-seek-presentation)
    # Playing originのseekは、transportを再開できて初めてPlayingへ戻る。
    mvm_add_p5d_product_test(preview_engine_p5d_exact_seek_playing ARGS --seek-playing)
    # resume失敗時にPlayingを一度も公開しないこと。
    mvm_add_p5d_product_test(preview_engine_p5d_seek_resume_fail_closed
        ARGS --fault-seek-resume)
    # seek completionで得たaudio generationをengineがenforceしていること。
    mvm_add_p5d_product_test(preview_engine_p5d_seek_audio_generation_fail_closed
        ARGS --fault-seek-audio-generation)
    # resume を barrier で止めている間、Playing を公開しないこと。
    mvm_add_p5d_product_test(preview_engine_p5d_seek_resume_barrier ARGS --seek-resume-barrier)
    # resume 中の requestShutdown() は cancellation であり、Error にしない。
    mvm_add_p5d_product_test(preview_engine_p5d_seek_shutdown_race ARGS --seek-shutdown-race)
    # P5-D4: phase5-plan.md §9.3「QPC fallback count 0」。他の11本は counter == 0 を
    # 要求する positive 側であり、これはその検査が効いていることを示す negative 側である。
    mvm_add_p5d_product_test(preview_engine_p5d_qpc_master_fail_closed ARGS --fault-qpc-master)

    # 対象0件のtest群を「全部通った」と報告させない (AGENTS.md)。
    list(LENGTH _mvm_p5d_product_tests _mvm_p5d_product_registered)
    if(NOT _mvm_p5d_product_registered EQUAL 13)
        message(FATAL_ERROR
            "[mvm] P5-D product testの登録数が不正です: ${_mvm_p5d_product_registered}/13")
    endif()

    # --- Phase 5 / P5-E: product composition --------------------------------
    # P5-E3: capability 2/2、多層render、per-source seek generation。
    set(_mvm_p5e_product_tests)
    macro(mvm_add_p5e_product_test name)
        cmake_parse_arguments(T "" "" "ARGS" ${ARGN})
        set(_command
            "${MVM_PWSH}" -NoProfile -File "${_p5c_fixture_wrapper}"
            -RequiredFile "${_p5c_fixture_a}" -AdditionalRequiredFile "${_p5e_fixture_b}"
            -Hint "${_p5c_fixture_hint}"
            -Exe "$<TARGET_FILE:mvm_p5e_preview_smoke>")
        if(T_ARGS)
            list(JOIN T_ARGS "|" _child_args)
            list(APPEND _command -ChildArgs "${_child_args}")
        endif()
        mvm_add_test(${name} COMMAND ${_command}
            PROPERTIES LABELS p5e TIMEOUT 90)
        set_tests_properties(${name} PROPERTIES LABELS "p5e;workstation")
        list(APPEND _mvm_p5e_product_tests ${name})
    endmacro()

    mvm_add_p5e_product_test(preview_engine_p5e_product_smoke)
    mvm_add_p5e_product_test(preview_engine_p5e_single_layer ARGS --single-layer)
    mvm_add_p5e_product_test(preview_engine_p5e_exceed_source_count
        ARGS --exceed-source-count)
    mvm_add_p5e_product_test(preview_engine_p5e_exceed_layer_count
        ARGS --exceed-layer-count)
    mvm_add_p5e_product_test(preview_engine_p5e_duplicate_source_layer
        ARGS --duplicate-source-layer)
    mvm_add_p5e_product_test(preview_engine_p5e_missing_pair_fail_closed
        ARGS --fault-missing-pair)
    mvm_add_p5e_product_test(preview_engine_p5e_remove_released_source
        ARGS --remove-released-source)
    mvm_add_p5e_product_test(preview_engine_p5e_seek_two_source ARGS --seek-two-source)
    mvm_add_p5e_product_test(preview_engine_p5e_seek_partial_generation_fail_closed
        ARGS --fault-seek-partial-generation)
    mvm_add_p5e_product_test(preview_engine_p5e_seek_partial_request_fail_closed
        ARGS --fault-seek-partial-request)
    # compose成立後・提示前にcomposition epoch authorityだけを進め、
    # `validateForDisplay()`が製品経路で効いていることを固定する。
    # この負例が無いと、render pathからvalidateForDisplay()を削除しても
    # 既存testが赤くならない。
    mvm_add_p5e_product_test(preview_engine_p5e_stale_composition_epoch
        ARGS --fault-stale-composition-epoch)
    # P5-E2: source removal guard。capabilityが1 video sourceのうちは、composition
    # 提出後にその参照が外れることはないので、削除成功は提出前の経路だけが到達可能。
    mvm_add_p5e_product_test(preview_engine_p5e_remove_unreferenced_source
        ARGS --remove-unreferenced-source)
    mvm_add_p5e_product_test(preview_engine_p5e_remove_referenced_source
        ARGS --remove-referenced-source)
    # authoritative audio sourceを安全に解放するとaudio authorityが空へ戻る。
    mvm_add_p5e_product_test(preview_engine_p5e_remove_audio_source
        ARGS --remove-audio-source)
    # audio sinkを停止できないままsourceを登録解除すると、鳴り続けているendpointの
    # ownerが居なくなる。sink自身にstopを失敗させ、通常のremoval経路を通す。
    mvm_add_p5e_product_test(preview_engine_p5e_remove_audio_stop_fail_closed
        ARGS --fault-remove-audio-stop)
    # audio停止フェーズ (engine lockを持たない窓) でfatalが入った場合、
    # removalをcommitしないというtransaction semanticsのauthority。
    mvm_add_p5e_product_test(preview_engine_p5e_remove_shutdown_race
        ARGS --remove-shutdown-race)
    # removal fatalのstate commitとevent挿入をlinearizeしていることのauthority。
    # commit直後・flush前で止め、その間にteardownを終端まで進めても
    # ShuttingDownがErrorを追い越されないことを決定論的に固定する。
    mvm_add_p5e_product_test(preview_engine_p5e_remove_fatal_event_order
        ARGS --remove-fatal-event-order)

    list(LENGTH _mvm_p5e_product_tests _mvm_p5e_product_registered)
    if(NOT _mvm_p5e_product_registered EQUAL 17)
        message(FATAL_ERROR
            "[mvm] P5-E product testの登録数が不正です: ${_mvm_p5e_product_registered}/17")
    endif()
endif()

add_executable(mvm_test_gpu_pure gpu_preview/test_gpu_pure.cpp)
target_link_libraries(mvm_test_gpu_pure PRIVATE mvm::gpu_preview mvm::warnings)
# mvm_add_test を使う。ucrt64in を PATH に入れないと
# 依存 DLL が解決できず 0xC0000135 で落ちる (実際に落ちた)。
mvm_add_test(gpu_pure_unit COMMAND mvm_test_gpu_pure)

# P2-Q4はproduction schedulerを変更せず、offline modelの契約を先に固定する。
add_executable(mvm_test_p2_render_opportunity
    harness/test_p2_render_opportunity_replay.cpp
    harness/p2_render_opportunity_replay.cpp
)
target_include_directories(mvm_test_p2_render_opportunity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/harness)
target_link_libraries(mvm_test_p2_render_opportunity PRIVATE mvm::warnings)
mvm_add_test(p2_render_opportunity_unit COMMAND mvm_test_p2_render_opportunity)

# P2-Q6はactual presentation opportunity ordinalからsource domainを閉じる
# pure/offline proofであり、production schedulerへは接続しない。
add_executable(mvm_test_p2_opportunity_ordinal
    harness/test_p2_opportunity_ordinal_replay.cpp
    harness/p2_opportunity_ordinal_replay.cpp
)
target_include_directories(mvm_test_p2_opportunity_ordinal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/harness)
target_link_libraries(mvm_test_p2_opportunity_ordinal PRIVATE mvm::warnings)
mvm_add_test(p2_opportunity_ordinal_unit COMMAND mvm_test_p2_opportunity_ordinal)

if(MVM_ENABLE_QT)
    add_executable(mvm_p2_scheduler_replay
        harness/p2_scheduler_replay.cpp
        harness/p2_render_opportunity_replay.cpp
    )
    target_include_directories(mvm_p2_scheduler_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/harness)
    target_link_libraries(mvm_p2_scheduler_replay PRIVATE
        mvm::gpu_preview
        Qt6::Core
        mvm::warnings
    )

    add_executable(mvm_p2_opportunity_ordinal_replay
        harness/p2_opportunity_ordinal_replay_cli.cpp
        harness/p2_opportunity_ordinal_replay.cpp
    )
    target_include_directories(mvm_p2_opportunity_ordinal_replay
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/harness)
    target_link_libraries(mvm_p2_opportunity_ordinal_replay PRIVATE Qt6::Core mvm::warnings)
endif()

add_executable(mvm_test_source_frame_buffer_interruptible_wait
    gpu_preview/test_source_frame_buffer_interruptible_wait.cpp)
target_link_libraries(mvm_test_source_frame_buffer_interruptible_wait PRIVATE
    mvm::gpu_preview mvm::warnings)
mvm_add_test(source_frame_buffer_interruptible_wait
    COMMAND mvm_test_source_frame_buffer_interruptible_wait)

# --- Phase 4 / A: composition schedule と snapshot identity foundation -------
add_executable(mvm_test_phase4a_foundation gpu_preview/test_phase4a_foundation.cpp)
target_link_libraries(mvm_test_phase4a_foundation PRIVATE mvm::gpu_preview mvm::warnings)
set(MVM_PHASE4A_TESTS
    CanonicalLayoutS0
    CanonicalLayoutS1
    CanonicalLayoutS2
    CanonicalLayoutS3
    CanonicalScheduleStrings
    CanonicalStateNames
    GoodFormalScheduleResolution
    GoodSmokeScheduleResolution
    RejectEmptySchedule
    RejectFirstBoundaryNonzero
    RejectNegativeBoundary
    RejectDuplicateBoundary
    RejectDescendingBoundary
    RejectInvalidState
    RejectUnresolvableFrame
    InitialStateSetup
    AdoptDifferentStateIncrementsEpochOnce
    AdoptSameStateIsNoop
    RejectedAdoptionDoesNotMutate
    RepeatedNoopDoesNotIncrementEpoch
    TransitionSequenceIncrementsFiveTimes
    ComposedFrameSnapshotIdentity
    DisplayLedgerSnapshotIdentity
    AtomicAdoptChangesStateLayoutEpochOnce
    AtomicAdoptSameSnapshotIsNoOp
    AtomicRejectInvalidStateDoesNotMutate
    AtomicRejectInvalidLayoutDoesNotMutate
    AtomicRejectSameStateDifferentLayout
    AtomicEpochOverflowDoesNotMutate
    AtomicSnapshotComposeMatchesStateAndLayout
    AtomicTransitionSequenceIncrementsFiveTimes
)
foreach(test_name IN LISTS MVM_PHASE4A_TESTS)
    mvm_add_test(phase4a_${test_name}
        COMMAND mvm_test_phase4a_foundation ${test_name})
endforeach()

# --- Phase 4 / B: schedule resolve -> atomic adoption -> pair -> ledger ------
add_executable(mvm_test_phase4b_integration gpu_preview/test_phase4b_integration.cpp)
target_link_libraries(mvm_test_phase4b_integration PRIVATE mvm::gpu_preview mvm::warnings)
set(MVM_PHASE4B_TESTS
    SmokeRunAdoptsExactlyTwice
    FormalLedgerRetainsAll3600
    FormalLedgerCapacityExceedsMeasurement
    BoundedLedgerEvictsOldest
    LedgerVerificationDetectsWrongState
    BoundaryDropStillActivatesNewState
    RepeatedSameFrameIsNoop
    UnconfiguredCoordinatorIsRejected
    UnresolvableFrameIsNotCountedAsResolve
    TransitionKeepsSourceGeneration
    ComposedLayoutMatchesScheduleState
)
foreach(test_name IN LISTS MVM_PHASE4B_TESTS)
    mvm_add_test(phase4b_${test_name}
        COMMAND mvm_test_phase4b_integration ${test_name})
endforeach()

# --- Phase 4 / B.1: freeze 済み shutdown 順 ---------------------------------
add_executable(mvm_test_phase4b_lifecycle gpu_preview/test_phase4b_lifecycle.cpp)
target_link_libraries(mvm_test_phase4b_lifecycle PRIVATE mvm::gpu_preview mvm::warnings)
set(MVM_PHASE4B_LIFECYCLE_TESTS
    SchedulerDisabledBeforeAudioStop
    AudioSinkStoppedBeforeAudioWorker
    AudioWorkerStoppedBeforeVideoWorkers
    WorkersJoinedBeforeRenderTeardown
    SharedRefsDetachedBeforeRenderTeardown
    FrozenOrderMatchesContract
    UnjoinedWorkerBlocksRenderTeardown
    MissingJoinCheckBlocksRenderTeardown
    MissingPipelinesStillReachesTeardown
)
foreach(test_name IN LISTS MVM_PHASE4B_LIFECYCLE_TESTS)
    mvm_add_test(phase4b_lifecycle_${test_name}
        COMMAND mvm_test_phase4b_lifecycle ${test_name})
endforeach()

# 4/B verifier の対照群と negative。verifier が壊した箇所で落ちることを見る。
set(_p4b_verifier_cases
    Good NegativeWrongState NegativeStaleEpoch NegativeAdoptionCount
    NegativeCounterSelfConsistency NegativeProbeFabricated NegativeProbeFieldMissing
    NegativeFormalHash NegativeLagOutOfRange NegativeGenerationChange
    NegativeLayerIdentity NegativeFalseSanityPass NegativeFirstFrame
    NegativeShutdownOrder NegativeShutdownMissingStep NegativeTeardownBeforeJoin)
foreach(_p4b_case IN LISTS _p4b_verifier_cases)
    string(TOLOWER "${_p4b_case}" _p4b_case_name)
    mvm_add_test(phase4b_verifier_${_p4b_case_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p4b-verifier.ps1"
            -Case ${_p4b_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/verify-p4-b-integration.ps1"
            -OutputDir "${CMAKE_CURRENT_BINARY_DIR}/p4-b-verifier-${_p4b_case_name}")
endforeach()

# --- Phase 4 / C: fail-closed smoke checker --------------------------------
set(_p4c_checker_cases
    Good SmokePerformanceDiagnosticOnly WrongProbeRgb WrongProbeAlpha ProbeMissing ProbeDuplicate ProbeWrongCoordinate
    ProbeWrongBoundary ProbeWrongActualFrame ProbeWrongState ProbeWrongEpoch ProbeCount3
    ProbeCount5 ProbeBlockingWait ProbePendingAfterDrain ProbeCompletionFailure
    ProbeUntrackedCompletion WrongCpuReference CpuReferenceWrongState WrongScheduleHash FormalScheduleInSmoke
    FormalCountInSmoke WrongState StaleEpoch OldStateAfterBoundary Lag3 GenerationChanged
    ShutdownOrder TeardownBeforeJoin MissingBoolean StringFalseBoolean AvRawBadSummaryGood
    SmokeFpsSummaryMismatch SmokeDropSummaryMismatch SmokeAvSummaryMismatch)
foreach(_p4c_case IN LISTS _p4c_checker_cases)
    string(TOLOWER "${_p4c_case}" _p4c_case_name)
    mvm_add_test(phase4c_checker_${_p4c_case_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p4-smoke-contract.ps1"
            -Case ${_p4c_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p4-smoke-contract.ps1"
            -OutputDir "${CMAKE_CURRENT_BINARY_DIR}/p4-c-checker-${_p4c_case_name}")
endforeach()

add_executable(mvm_test_phase4c_probe gpu_preview/test_phase4c_probe.cpp)
target_link_libraries(mvm_test_phase4c_probe PRIVATE mvm::gpu_preview mvm::warnings)
set(_p4c_probe_cases CandidateFrameSelection DuplicateTransitionSuppression
    FormalBoundaryConfiguration
    ExpectedLocationsAndOpacity InvalidStateHasNoProbeExpectation VisibleUvNormalization
    Bt709Reference StraightAlpha ToleranceAndAlpha AsyncGpuCopy)
foreach(_p4c_probe_case IN LISTS _p4c_probe_cases)
    string(TOLOWER "${_p4c_probe_case}" _p4c_probe_case_name)
    mvm_add_test(phase4c_probe_${_p4c_probe_case_name}
        COMMAND mvm_test_phase4c_probe ${_p4c_probe_case})
endforeach()
set(_p4_probe_fixture_a "${CMAKE_SOURCE_DIR}/tests/assets/p3_audio/p3_av_h264_aac.mp4")
set(_p4_probe_fixture_b "${CMAKE_SOURCE_DIR}/tests/assets/p3_audio/p3_video_hevc_b.mp4")
if(EXISTS "${_p4_probe_fixture_a}" AND EXISTS "${_p4_probe_fixture_b}")
    mvm_add_test(phase4c_probe_fixturereference
        COMMAND mvm_test_phase4c_probe FixtureReference
            "${_p4_probe_fixture_a}" "${_p4_probe_fixture_b}"
        PROPERTIES REQUIRED_FILES "${_p4_probe_fixture_a}"
            TIMEOUT 120)
    mvm_add_test(phase4d_probe_formal_fixturereference
        COMMAND mvm_test_phase4c_probe FormalFixtureReference
            "${_p4_probe_fixture_a}" "${_p4_probe_fixture_b}"
        PROPERTIES REQUIRED_FILES "${_p4_probe_fixture_a}"
            TIMEOUT 180)
else()
    message(STATUS "[mvm] P4-C/D fixture referenceは未登録です。")
    message(STATUS "  pwsh scripts/make-p3-fixture.ps1 を実行してください。")
endif()
mvm_add_test(phase4c_nonblocking_architecture
    COMMAND "${MVM_PWSH}" -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-phase4c-nonblocking-architecture.ps1"
        -SourceRoot "${CMAKE_SOURCE_DIR}")

# --- Phase 4 / D1: formal checker / 3-process matrix harness ----------------
set(_p4d_formal_cases
    GoodFormal WrongFormalSchedule WrongFormalHash SmokeScheduleInFormal WrongWarmup4
    WrongDuration59 WrongRequiredFrames WrongAudioEndSample Adoption4 Adoption6
    EpochIncrement4 Reject1 WrongState StaleEpoch WrongReturnToS0EpochAt2400
    WrongReturnToS1EpochAt3000 Lag3 MissingLag LagStringZero LagDoubleZero LagBoolean
    OldStateAfterBoundary ProbeCount9
    ProbeCount11 ProbeMissing ProbeDuplicate ProbeWrongFrame ProbeWrongState ProbeWrongEpoch
    CpuReferenceWrongState WrongProbeRgb WrongProbeAlpha ProbeActualRgbString
    ProbeExpectedRgbString ProbeRgbaNull ProbeRgbaOutOfRange ProbeBlockingWait ProbePending
    ProbeCompletionFailure GenerationChanged ResourceEpochChanged FpsBelow55 DropOver2Percent
    AvP95_20_001 AvMax_33_335 AvProjectionFalse AvMissingRaw AvNaN
    AvSummaryDoesNotMatchRaw ShutdownOrder TeardownBeforeJoin MissingBoolean
    StringFalseBoolean ProcessExitNonzero WrongAudioSampleRate WrongAudioChannels
    WrongAudioSampleFormat)
foreach(_case IN LISTS _p4d_formal_cases)
    string(TOLOWER "${_case}" _case_name)
    mvm_add_test(phase4d_formal_checker_${_case_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p4-formal-contract.ps1"
            -Case ${_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p4-formal-contract.ps1"
            -Generator "${CMAKE_SOURCE_DIR}/scripts/new-p4-formal-synthetic.ps1"
            -OutputDir "${CMAKE_CURRENT_BINARY_DIR}/p4-d-formal-${_case_name}")
endforeach()

foreach(_case GoodDryRun DetachedHead OneRunFailure DirtyBaseline ProvenanceChange MissingRaw InvalidRuntimeBin)
    string(TOLOWER "${_case}" _case_name)
    mvm_add_test(phase4d_matrix_${_case_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p4-formal-matrix.ps1"
            -Case ${_case}
            -Harness "${CMAKE_SOURCE_DIR}/scripts/run-p4-formal-matrix.ps1"
            -OutputDir "${CMAKE_CURRENT_BINARY_DIR}/p4-d-matrix-${_case_name}"
            -GitExe "${GIT_EXECUTABLE}")
endforeach()

if(TARGET mvm_p4_composition_spike)
    mvm_add_test(phase4d_runtime_reject_unknown_workload
        COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/expect-exit.ps1"
            -ExpectExit 2 -Exe "$<TARGET_FILE:mvm_p4_composition_spike>"
            -ChildArgs "--workload|unknown")
    mvm_add_test(phase4d_runtime_reject_duration_override
        COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/expect-exit.ps1"
            -ExpectExit 2 -Exe "$<TARGET_FILE:mvm_p4_composition_spike>"
            -ChildArgs "--duration-seconds|60")
endif()

# --- Phase 3 / A: audio pipeline --------------------------------------------
add_executable(mvm_test_audio_pure audio_preview/test_audio_pure.cpp)
target_link_libraries(mvm_test_audio_pure PRIVATE mvm::audio_preview mvm::warnings)
mvm_add_test(p3_audio_pure_unit COMMAND mvm_test_audio_pure)
set_tests_properties(p3_audio_pure_unit PROPERTIES LABELS p5d1)
list(APPEND _mvm_p5d1_tests p3_audio_pure_unit)

list(LENGTH _mvm_p5d1_tests _mvm_p5d1_registered)
if(NOT _mvm_p5d1_registered EQUAL 3)
    message(FATAL_ERROR
        "[mvm] P5-D1 testの登録数が不正です: ${_mvm_p5d1_registered}/3")
endif()
message(STATUS "[mvm] P5-D1 testを${_mvm_p5d1_registered}件登録しました")

add_executable(mvm_p3_audio_smoke audio_preview/mvm_p3_audio_smoke.cpp)
target_link_libraries(mvm_p3_audio_smoke PRIVATE mvm::audio_preview mvm::warnings)

add_executable(mvm_test_audio_worker_wake audio_preview/test_audio_worker_wake.cpp)
target_link_libraries(mvm_test_audio_worker_wake PRIVATE mvm::audio_preview mvm::warnings)

add_executable(mvm_test_display_target_contract
    audio_preview/test_display_target_contract.cpp)
target_link_libraries(mvm_test_display_target_contract PRIVATE mvm::preview_qt mvm::warnings)
mvm_add_test(p3_c2_display_target_preflight_unit
    COMMAND mvm_test_display_target_contract)

foreach(_p5e_preflight_case Good NegativeScreenName)
    string(TOLOWER "${_p5e_preflight_case}" _p5e_preflight_case_name)
    mvm_add_test(p5_e4_display_preflight_${_p5e_preflight_case_name}
        COMMAND pwsh -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/audio_preview/test-p5-e4-display-preflight.ps1"
            -Case ${_p5e_preflight_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p5-e4-display-preflight.ps1"
            -OutputDir "${CMAKE_CURRENT_BINARY_DIR}/p5-e4-display-preflight-${_p5e_preflight_case_name}")
endforeach()

mvm_add_test(p5_e4_a1_classifier_unit
    COMMAND pwsh -NoProfile -File
        "${CMAKE_SOURCE_DIR}/scripts/p5-e4-a1-eof-tail.ps1" -SelfTest)

foreach(_p3_case Good NegativeCounter)
    string(TOLOWER "${_p3_case}" _p3_case_name)
    mvm_add_test(p3_a_contract_${_p3_case_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/audio_preview/test-p3-a-contract.ps1"
            -Case ${_p3_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p3-a-contract.ps1"
            -OutputDir "${CMAKE_CURRENT_BINARY_DIR}/p3-a-contract-${_p3_case_name}")
endforeach()

foreach(_p3b_case Good NegativeFallback)
    string(TOLOWER "${_p3b_case}" _p3b_case_name)
    mvm_add_test(p3_b_contract_${_p3b_case_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/audio_preview/test-p3-b-contract.ps1"
            -Case ${_p3b_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p3-b-contract.ps1"
            -OutputDir "${CMAKE_CURRENT_BINARY_DIR}/p3-b-contract-${_p3b_case_name}")
endforeach()

set(_p3c_contract_cases
    Good
    NegativeFps NegativeDrop NegativeAvP95 NegativeAvMax NegativeAvCount
    NegativeScheduledAccounting NegativeFirstFrame NegativeAudioStartSample
    NegativeUnderflow NegativeOverflow NegativeMarker NegativeMixedPair
    NegativeGeneration NegativeAhead NegativeClockRegression NegativeQpcFallback
    NegativeSeekCount999 NegativeSeekMismatch NegativeSeekTimeout NegativeSeekP95
    NegativeSeekMax NegativePauseAdvance NegativePauseClock NegativeNaN
    NegativeMissingProperty)
foreach(_p3c_case IN LISTS _p3c_contract_cases)
    string(TOLOWER "${_p3c_case}" _p3c_case_name)
    mvm_add_test(p3_c_contract_${_p3c_case_name}
        COMMAND pwsh -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/audio_preview/test-p3-c-contract.ps1"
            -Case ${_p3c_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p3-c-contract.ps1"
            -OutputDir "${CMAKE_CURRENT_BINARY_DIR}/p3-c-contract-${_p3c_case_name}")
endforeach()

set(_p3c2_contract_cases
    GoodC2 NegativePreflightFalse NegativePreflightType
    NegativeWorkloadFalse NegativeWorkloadType
    NegativeMissingDisplayTelemetry NegativeRequestedWidth
    NegativeWindowWidth NegativeWindowHeight NegativeSurfaceWidth NegativeSurfaceHeight
    NegativeRhiWidth NegativeRhiHeight NegativeDpr
    NegativeDisplayStartEndOrientation NegativeDisplayStartEndGeometry
    NegativeDisplayStartEndDpr NegativeDisplayStartEndWindow
    NegativeDisplayStartEndSurface NegativeDisplayStartEndRhiTarget
    NegativeDisplayEnvironmentRunMismatch NegativeDisplayNull NegativeDisplayNaN
    NegativeDisplayType)
foreach(_p3c2_case IN LISTS _p3c2_contract_cases)
    string(TOLOWER "${_p3c2_case}" _p3c2_case_name)
    mvm_add_test(p3_c2_contract_${_p3c2_case_name}
        COMMAND pwsh -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/audio_preview/test-p3-c2-contract.ps1"
            -Case ${_p3c2_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p3-c2-contract.ps1"
            -LegacyChecker "${CMAKE_SOURCE_DIR}/scripts/check-p3-c-contract.ps1"
            -LegacyFixtureScript
                "${CMAKE_CURRENT_SOURCE_DIR}/audio_preview/test-p3-c-contract.ps1"
            -OutputDir "${CMAKE_CURRENT_BINARY_DIR}/p3-c2-contract-${_p3c2_case_name}")
endforeach()

set(MVM_P3_AUDIO_FIXTURE "${CMAKE_SOURCE_DIR}/tests/assets/p3_audio/p3_av_h264_aac.mp4")
if(EXISTS "${MVM_P3_AUDIO_FIXTURE}")
    mvm_add_test(p3_audio_fixture_decode
        COMMAND mvm_p3_audio_smoke fixture "${MVM_P3_AUDIO_FIXTURE}"
                "${CMAKE_CURRENT_BINARY_DIR}/p3-a-fixture.json"
        PROPERTIES REQUIRED_FILES "${MVM_P3_AUDIO_FIXTURE}" TIMEOUT 120)
    mvm_add_test(p3_audio_worker_wake_soak_short
        COMMAND mvm_test_audio_worker_wake "${MVM_P3_AUDIO_FIXTURE}" 50
        PROPERTIES REQUIRED_FILES "${MVM_P3_AUDIO_FIXTURE}" TIMEOUT 180)
    mvm_add_test(p3_audio_worker_wake_soak_1000
        COMMAND mvm_test_audio_worker_wake "${MVM_P3_AUDIO_FIXTURE}" 1000
        PROPERTIES REQUIRED_FILES "${MVM_P3_AUDIO_FIXTURE}" LABELS performance TIMEOUT 1800)
else()
    message(STATUS
        "[mvm] P3-A fixture 未生成のため decode integration は未登録です。\n"
        "  pwsh scripts/make-p3-fixture.ps1 を実行してください。")
endif()

set(MVM_P3_B_VIDEO_FIXTURE "${CMAKE_SOURCE_DIR}/tests/assets/p3_audio/p3_video_hevc_b.mp4")
if(TARGET mvm_p3_av_sync_spike AND EXISTS "${MVM_P3_AUDIO_FIXTURE}" AND
   EXISTS "${MVM_P3_B_VIDEO_FIXTURE}")
    mvm_add_test(p3_b_playback_integration_short
        COMMAND mvm_p3_av_sync_spike
            --source-a "${MVM_P3_AUDIO_FIXTURE}" --source-b "${MVM_P3_B_VIDEO_FIXTURE}"
            --metrics "${CMAKE_CURRENT_BINARY_DIR}/p3-b-playback-short.json"
            --mode playback --duration-seconds 3
        PROPERTIES RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 180)
    mvm_add_test(p3_b_pause_resume_integration
        COMMAND mvm_p3_av_sync_spike
            --source-a "${MVM_P3_AUDIO_FIXTURE}" --source-b "${MVM_P3_B_VIDEO_FIXTURE}"
            --metrics "${CMAKE_CURRENT_BINARY_DIR}/p3-b-pause.json" --mode pause-resume
        PROPERTIES RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 180)
    mvm_add_test(p3_b_seek_integration_short
        COMMAND mvm_p3_av_sync_spike
            --source-a "${MVM_P3_AUDIO_FIXTURE}" --source-b "${MVM_P3_B_VIDEO_FIXTURE}"
            --metrics "${CMAKE_CURRENT_BINARY_DIR}/p3-b-seek-short.json"
            --mode seek --seek-count 4
        PROPERTIES RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 300)
    mvm_add_test(p3_async_audio_failure_fail_close
        COMMAND pwsh -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/audio_preview/test-p3-async-audio-failure.ps1"
            -Executable "$<TARGET_FILE:mvm_p3_av_sync_spike>"
            -SourceA "${MVM_P3_AUDIO_FIXTURE}" -SourceB "${MVM_P3_B_VIDEO_FIXTURE}"
            -OutputDir "${CMAKE_CURRENT_BINARY_DIR}/p3-async-audio-failure"
        PROPERTIES RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 120)
else()
    message(STATUS
        "[mvm] P3-B fixture 未生成のため integration は未登録です。\n"
        "  pwsh scripts/make-p3-fixture.ps1 を実行してください。")
endif()

# --- Phase 4 / C: 10秒 contract smoke ---------------------------------------
# checkerだけがsmoke PASS authority。formal_verdictは常にNOT_RUN。
set(MVM_P4_C_JSON "${CMAKE_CURRENT_BINARY_DIR}/p4-c-smoke.json")
if(TARGET mvm_p4_composition_spike AND EXISTS "${MVM_P3_AUDIO_FIXTURE}" AND
   EXISTS "${MVM_P3_B_VIDEO_FIXTURE}")
    mvm_add_test(p4_c_contract_smoke
        COMMAND mvm_p4_composition_spike
            --source-a "${MVM_P3_AUDIO_FIXTURE}" --source-b "${MVM_P3_B_VIDEO_FIXTURE}"
            --metrics "${MVM_P4_C_JSON}" --workload smoke
        PROPERTIES RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 300
            FIXTURES_SETUP mvm_p4_c)
    mvm_add_test(p4_c_contract_smoke_check
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_SOURCE_DIR}/scripts/check-p4-smoke-contract.ps1" -Json "${MVM_P4_C_JSON}"
        PROPERTIES FIXTURES_REQUIRED mvm_p4_c TIMEOUT 120)
else()
    message(STATUS
        "[mvm] P4-C fixture 未生成のため contract smoke は未登録です。\n"
        "  pwsh scripts/make-p3-fixture.ps1 を実行してください。")
endif()

add_executable(mvm_test_gpu_decode gpu_preview/test_gpu_decode.cpp)
target_link_libraries(mvm_test_gpu_decode PRIVATE mvm::gpu_preview mvm::util mvm::warnings)

add_executable(mvm_test_p2_dual_decode gpu_preview/test_p2_dual_decode.cpp)
target_link_libraries(mvm_test_p2_dual_decode PRIVATE mvm::gpu_preview mvm::warnings)
mvm_add_test(source_decode_worker_test_barrier_stop
    COMMAND mvm_test_p2_dual_decode --barrier-stop
            "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
        RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 120)
add_executable(mvm_test_p2_gpu_compositor gpu_preview/test_p2_gpu_compositor.cpp)
target_link_libraries(mvm_test_p2_gpu_compositor PRIVATE mvm::gpu_preview mvm::warnings)

# deterministic fixture はリポジトリ管理なので常に必須。
# 60秒 benchmark 動画は通常CIでは生成しないため、それを使う統合テストだけを
# 条件付き登録する。Smoke 素材へ置き換えると frame 3599 の検査が空振りする。
set(MVM_P2_COMPOSITOR_FIXTURE "${CMAKE_SOURCE_DIR}/tests/assets/p2_compositor/manifest.txt")
if(NOT EXISTS "${MVM_P2_COMPOSITOR_FIXTURE}")
    message(FATAL_ERROR "[mvm] P2 deterministic fixtureがありません: ${MVM_P2_COMPOSITOR_FIXTURE}")
endif()

set(MVM_HAVE_P2_BENCHMARK_FIXTURES TRUE)
foreach(_p2_video v1080p60_h264.mp4 v1080p60_hevc.mp4)
    if(NOT EXISTS "${MVM_BENCH_DIR}/${_p2_video}")
        set(MVM_HAVE_P2_BENCHMARK_FIXTURES FALSE)
    endif()
endforeach()

if(MVM_HAVE_P2_BENCHMARK_FIXTURES)
    mvm_add_test(p2_dual_decode_integration
        COMMAND mvm_test_p2_dual_decode
                "${MVM_BENCH_DIR}/v1080p60_h264.mp4"
                "${MVM_BENCH_DIR}/v1080p60_hevc.mp4"
        PROPERTIES RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 900)

    mvm_add_test(p2_gpu_compositor_offscreen
        COMMAND mvm_test_p2_gpu_compositor
                "${MVM_BENCH_DIR}/v1080p60_h264.mp4"
                "${MVM_BENCH_DIR}/v1080p60_hevc.mp4"
        PROPERTIES RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 900)
else()
    message(STATUS
        "[mvm] P2 benchmark統合テストは未登録です。\n"
        "  実行するには pwsh scripts/make-testmedia.ps1 -Mode Benchmark")
endif()

# --- Phase 2 / C2: actual QQuickRhiItem target ------------------------------
set(MVM_C2_PLAYBACK_JSON "${CMAKE_CURRENT_BINARY_DIR}/c2-playback.json")
set(MVM_C2_SEEK_JSON "${CMAKE_CURRENT_BINARY_DIR}/c2-seek.json")
set(MVM_C2_LAYOUT_JSON "${CMAKE_CURRENT_BINARY_DIR}/c2-layout.json")

if(MVM_HAVE_P2_BENCHMARK_FIXTURES)
    mvm_add_test(compositor_qt_two_layers
        COMMAND mvm_compositor_spike
            --source-a "${MVM_BENCH_DIR}/v1080p60_h264.mp4"
            --source-b "${MVM_BENCH_DIR}/v1080p60_hevc.mp4"
            --warmup-seconds 1 --measure-seconds 2 --seed 20260808 --seek-count 64
            --metrics "${MVM_C2_PLAYBACK_JSON}" --gpu-completion fence
            --display-timeout-ms 2000 --mode playback
        PROPERTIES RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 120
            FIXTURES_SETUP mvm_c2_playback)
endif()

# P2-D5-1のdispatch順序はGPU非依存のpure helperで固定する。
add_executable(mvm_test_parallel_dispatch_order gpu_preview/test_parallel_dispatch_order.cpp)
target_include_directories(mvm_test_parallel_dispatch_order PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(mvm_test_parallel_dispatch_order PRIVATE mvm::warnings)
mvm_add_test(p2_parallel_dispatch_order COMMAND mvm_test_parallel_dispatch_order)

# P2-D5-2のcausal opportunity schedulerをGPU非依存で固定する。
add_executable(mvm_test_presentation_opportunity_scheduler
    gpu_preview/test_presentation_opportunity_scheduler.cpp)
target_link_libraries(mvm_test_presentation_opportunity_scheduler
    PRIVATE mvm::gpu_preview mvm::warnings)
mvm_add_test(p2_presentation_opportunity_scheduler
    COMMAND mvm_test_presentation_opportunity_scheduler)

# P2-D5-2/F3のwindow output VBlank authorityをGPU非依存のpure helperで固定する。
add_executable(mvm_test_window_output_vblank_authority
    gpu_preview/test_window_output_vblank_authority.cpp)
target_link_libraries(mvm_test_window_output_vblank_authority
    PRIVATE mvm::gpu_preview mvm::warnings)
mvm_add_test(p2_window_output_vblank_authority
    COMMAND mvm_test_window_output_vblank_authority)

# P2-D5-2-W2-A: measurement窓に対するphysical VBlank domainのexact構築。
# boundary / property / mutationをGPU非依存で固定する。
add_executable(mvm_test_physical_vblank_domain
    gpu_preview/test_physical_vblank_domain.cpp)
target_link_libraries(mvm_test_physical_vblank_domain
    PRIVATE mvm::gpu_preview mvm::warnings)
mvm_add_test(p2_d5_2_w2a_physical_vblank_domain
    COMMAND mvm_test_physical_vblank_domain)

# P2-D5-2/F3-A: window output VBlank authority probeのcontract。
# 対照群と、1 fieldだけを壊したnegativeで固定する。DWM composition clockが
# window outputと一致しない場合でもprobeが通ること (diagnostic-only) も含む。
foreach(_p2_vblank_case Good NegativeProbeFail NegativeOutputChanged NegativeSequenceStatus
        NegativeLongInterval NegativeWaitFailure NegativeRingOverflow
        NegativeCadenceDeviation NegativeDeviationRecalculation
        DwmClockMismatchIsDiagnosticOnly)
    string(TOLOWER "${_p2_vblank_case}" _p2_vblank_name)
    mvm_add_test(p2_vblank_authority_checker_${_p2_vblank_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-vblank-authority-contract.ps1"
            -Case ${_p2_vblank_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-vblank-authority.ps1"
            -Output "${CMAKE_CURRENT_BINARY_DIR}/p2-vblank-${_p2_vblank_name}.json")
endforeach()

# P2-D5-2/F3-B0: swap↔physical VBlankのshadow mapping contract。
foreach(_p2_shadow_case Good SupersedeGood NegativeAmbiguousSwap NegativeObserverGap
        NegativeAfterLast NegativeBeforeFirst NegativeShortInterval NegativeLongInterval
        NegativeCumulativeDrift NegativeNormalPriority NegativeOutputChanged
        NegativeSampleCount)
    string(TOLOWER "${_p2_shadow_case}" _p2_shadow_name)
    mvm_add_test(p2_vblank_shadow_checker_${_p2_shadow_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-vblank-shadow-contract.ps1"
            -Case ${_p2_shadow_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-vblank-shadow.ps1"
            -Output "${CMAKE_CURRENT_BINARY_DIR}/p2-vblank-shadow-${_p2_shadow_name}.json")
endforeach()

# 実displayとDXGI outputを要求するため workstation label を付ける。
if(TARGET mvm_p2_vblank_authority_probe)
    add_test(NAME p2_vblank_authority_probe_live
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/run-p2-vblank-authority-probe.ps1"
            -Exe "$<TARGET_FILE:mvm_p2_vblank_authority_probe>"
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-vblank-authority.ps1"
            -Output "${CMAKE_CURRENT_BINARY_DIR}/p2-vblank-authority-probe-live.json"
            -DurationMs 4000)
    set_tests_properties(p2_vblank_authority_probe_live PROPERTIES
        ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}"
        LABELS "p2;workstation" RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 120)
endif()

# P2-D5-2/F3-B0.6-R1: Present IDをjoin keyにする完全oracle contract。
# producerの真偽値だけに依存せず、submission列・statistics列・offline delayを再計算する。
foreach(_p2_oracle_case Good NegativeSubmittedGap NegativeObservedGap NegativeFinalDrain
        NegativePollInterval NegativeOfflineDelay NegativeEarlyMapper NegativeDiscardClaim)
    string(TOLOWER "${_p2_oracle_case}" _p2_oracle_name)
    mvm_add_test(p2_present_id_oracle_checker_${_p2_oracle_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-present-id-oracle-contract.ps1"
            -Case ${_p2_oracle_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-present-id-oracle.ps1"
            -Output "${CMAKE_CURRENT_BINARY_DIR}/p2-present-id-oracle-${_p2_oracle_name}.json")
endforeach()

if(TARGET mvm_p2_present_identity_probe)
    add_test(NAME p2_present_id_oracle_live
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/run-p2-present-id-oracle.ps1"
            -Exe "$<TARGET_FILE:mvm_p2_present_identity_probe>"
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-present-id-oracle.ps1"
            -Output "${CMAKE_CURRENT_BINARY_DIR}/p2-present-id-oracle-live.json"
            -Presents 900)
    set_tests_properties(p2_present_id_oracle_live PROPERTIES
        ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}"
        LABELS "p2;workstation" RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 180)
endif()

# P2-D5-2/F3-B0.6-R2: ETW event historyとapp swap sequenceのexact join contract。
foreach(_p2_etw_oracle_case Good NegativeCount NegativeOrder NegativeSwapChain
        NegativeSyncInterval NegativeWindow NegativeLostEvent NegativeRawQpc NegativeBoundary
        NegativePhysicalAuthority CollisionNotObservedGood)
    string(TOLOWER "${_p2_etw_oracle_case}" _p2_etw_oracle_name)
    mvm_add_test(p2_etw_present_history_checker_${_p2_etw_oracle_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-etw-present-history-contract.ps1"
            -Case ${_p2_etw_oracle_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-etw-present-history.ps1"
            -VBlankChecker "${CMAKE_SOURCE_DIR}/scripts/check-p2-vblank-shadow.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-etw-oracle-${_p2_etw_oracle_name}")
endforeach()

# F3-B0.6-R3: corpus constructionをmapperから独立にfail-closed検査する。
foreach(_p2_r3_case Good NegativeOracleOpportunity NegativeDelayTouchesDisplayed
        NegativeRecordOrder NegativeVBlankDeleted NegativeCollisionClaim)
    string(TOLOWER "${_p2_r3_case}" _p2_r3_name)
    mvm_add_test(p2_etw_synthetic_corpus_checker_${_p2_r3_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-etw-synthetic-corpus-contract.ps1"
            -Case ${_p2_r3_case}
            -CorpusChecker "${CMAKE_SOURCE_DIR}/scripts/check-p2-etw-synthetic-corpus.ps1"
            -OracleChecker "${CMAKE_SOURCE_DIR}/scripts/check-p2-etw-present-history.ps1"
            -VBlankChecker "${CMAKE_SOURCE_DIR}/scripts/check-p2-vblank-shadow.ps1"
            -Directory "${CMAKE_CURRENT_BINARY_DIR}/p2-r3-${_p2_r3_name}")
endforeach()

# P2-D5-1 formal checkerはGPU非依存の対照群と、1 fieldだけを壊したnegativeで固定する。
foreach(_p2_contract_case Good FormalGood NegativeInvariant NegativeSameDevice NegativeOutputSize
        NegativeBackend NegativeSeed NegativeFormalTiming NegativeScheduledHigh
        NegativeScheduledLow NegativeCoverage NegativeSourceFrameCount NegativeMissingPair
        NegativeEofA NegativeEofB NegativePrerollConfigured NegativePrerollOk
        NegativePrerollDepthA NegativePrerollDepthB NegativePrerollFrontA
        NegativePrerollFrontB SeekGood NegativeSeekPublishReject
        NegativeSeekRequestMismatch NegativeSeekStaleCompletion
        NegativeSeekBusyAcceptance NegativeSeekStoppedSuperseded
        NegativeSeekConcurrencyCount SeekExecutionNonoverlapGood
        FormalSeekGood NegativeParallelDispatchCount
        NegativeParallelDispatchSample NegativeBRequestResult
        NegativeDispatchAfterFirstReady NegativeOpportunityLedgerMutation
        NegativeOpportunityOrdinalGapIgnored NegativeOpportunityRefreshChange
        NegativeOpportunityAuthority NegativeOpportunityTail ReconciledGood
        NegativeForwardDropIgnored NegativeMeasurementDelta
        SupersededGood NegativeOpportunitySupersededSwapOrdinal
        NegativeOpportunityFirstCandidate NegativeOpportunityDisplayedCount
        NegativeOpportunityOrigin)
    string(TOLOWER "${_p2_contract_case}" _p2_contract_name)
    mvm_add_test(p2_formal_checker_${_p2_contract_name}
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/test-p2-contract.ps1"
            -Case ${_p2_contract_case}
            -Checker "${CMAKE_SOURCE_DIR}/scripts/check-p2-contract.ps1"
            -Output "${CMAKE_CURRENT_BINARY_DIR}/p2-contract-${_p2_contract_name}.json")
endforeach()

# 診断case typoを通常playbackへ暗黙fallbackさせない。
add_test(NAME compositor_diagnostic_case_invalid_negative
    COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/expect-exit.ps1"
        -ExpectExit 2 -Exe "$<TARGET_FILE:mvm_compositor_spike>"
        -ChildArgs "--diagnostic-case|invalid")
set_tests_properties(compositor_diagnostic_case_invalid_negative PROPERTIES
    ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}" TIMEOUT 30)

if(MVM_HAVE_P2_BENCHMARK_FIXTURES)
    foreach(_kind_name actual_device no_full_frame_copy actual_target_probe shutdown)
        if(_kind_name STREQUAL "actual_device")
            set(_kind device)
        elseif(_kind_name STREQUAL "no_full_frame_copy")
            set(_kind copy)
        elseif(_kind_name STREQUAL "actual_target_probe")
            set(_kind probe)
        else()
            set(_kind shutdown)
        endif()
        mvm_add_test(compositor_qt_${_kind_name}
            COMMAND "${MVM_PWSH}" -NoProfile -File
                "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/check-c2-metrics.ps1"
                -Json "${MVM_C2_PLAYBACK_JSON}" -Kind ${_kind}
            PROPERTIES FIXTURES_REQUIRED mvm_c2_playback)
    endforeach()

    mvm_add_test(compositor_qt_dual_seek_64
        COMMAND mvm_compositor_spike
            --source-a "${MVM_BENCH_DIR}/v1080p60_h264.mp4"
            --source-b "${MVM_BENCH_DIR}/v1080p60_hevc.mp4"
            --warmup-seconds 0 --measure-seconds 1 --seed 20260808 --seek-count 64
            --metrics "${MVM_C2_SEEK_JSON}" --gpu-completion fence
            --display-timeout-ms 2000 --mode seek
        PROPERTIES RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 300)
    mvm_add_test(compositor_qt_dual_seek_64_contract
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/check-c2-metrics.ps1"
            -Json "${MVM_C2_SEEK_JSON}" -Kind seek
        PROPERTIES DEPENDS compositor_qt_dual_seek_64)

    mvm_add_test(compositor_qt_layout_epoch
        COMMAND mvm_compositor_spike
            --source-a "${MVM_BENCH_DIR}/v1080p60_h264.mp4"
            --source-b "${MVM_BENCH_DIR}/v1080p60_hevc.mp4"
            --warmup-seconds 0 --measure-seconds 1 --seed 20260808 --seek-count 1
            --metrics "${MVM_C2_LAYOUT_JSON}" --gpu-completion fence
            --display-timeout-ms 2000 --mode layout
        PROPERTIES RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 120)
    mvm_add_test(compositor_qt_layout_epoch_contract
        COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/check-c2-metrics.ps1"
            -Json "${MVM_C2_LAYOUT_JSON}" -Kind layout
        PROPERTIES DEPENDS compositor_qt_layout_epoch)

    foreach(_fault device_change completion_fatal)
        set(_fault_json "${CMAKE_CURRENT_BINARY_DIR}/c2-${_fault}.json")
        add_test(NAME compositor_qt_${_fault}_fail_closed
            COMMAND "${MVM_PWSH}" -NoProfile -File
                "${CMAKE_CURRENT_SOURCE_DIR}/gpu_preview/run-c2-negative.ps1"
                -Exe "$<TARGET_FILE:mvm_compositor_spike>"
                -SourceA "${MVM_BENCH_DIR}/v1080p60_h264.mp4"
                -SourceB "${MVM_BENCH_DIR}/v1080p60_hevc.mp4"
                -Metrics "${_fault_json}" -Fault ${_fault})
        set_tests_properties(compositor_qt_${_fault}_fail_closed PROPERTIES
            ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}"
            RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 120)
    endforeach()
endif()

# 実 GPU を使う検査は終了コードを分けている。
# 0 以外なら合格ではなく、**意図した理由で失敗したこと**を照合する。
function(mvm_add_gpu_exit_test name)
    cmake_parse_arguments(T "" "EXIT" "ARGS;PROPERTIES" ${ARGN})
    if(NOT DEFINED T_EXIT)
        message(FATAL_ERROR "mvm_add_gpu_exit_test(${name}) に EXIT がありません")
    endif()
    list(JOIN T_ARGS "|" _child_args)
    add_test(NAME ${name}
        COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/expect-exit.ps1"
                -ExpectExit ${T_EXIT} -Exe "$<TARGET_FILE:mvm_test_gpu_decode>"
                -ChildArgs "${_child_args}")
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}"
        ${T_PROPERTIES})
endfunction()

# 正常系: H.264 / HEVC の両方で GPU frame が出る。
foreach(gpu_asset v1080p60_h264 v1080p60_hevc)
    mvm_add_test(gpu_decode_${gpu_asset}
        COMMAND mvm_test_gpu_decode decode "${MVM_SMOKE_DIR}/${gpu_asset}.mp4" 10
        PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/${gpu_asset}.mp4" TIMEOUT 120)

    mvm_add_test(gpu_seek_flush_${gpu_asset}
        COMMAND mvm_test_gpu_decode seek "${MVM_SMOKE_DIR}/${gpu_asset}.mp4" 137,0,299,42
        PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/${gpu_asset}.mp4" TIMEOUT 120)

    mvm_add_test(gpu_marker_${gpu_asset}
        COMMAND mvm_test_gpu_decode marker "${MVM_SMOKE_DIR}/${gpu_asset}.mp4" 0,1,137,299
        PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/${gpu_asset}.mp4" TIMEOUT 120)
endforeach()

# 回帰テスト: seek の着地は **完全一致**でなければならない。
#
# [事実] AVSEEK_FLAG_BACKWARD は「目標以前のキーフレーム」を保証しない。
# benchmark の 1080p60 HEVC で frame 299 を要求して 300 に着地し、
# 1000 点中 65 点 (6.5%) が同じ形で外れた。同じ time_base / fps の
# H.264 では 1 件も起きない。B frame 構造の差である。
#
# 「>= なら成功」にすると 1 frame ずれを飲み込み、編集点がずれる。
# 60 秒素材があるときだけ登録する (smoke の 5 秒素材では再現しない)。
if(EXISTS "${MVM_BENCH_DIR}/v1080p60_hevc.mp4")
    mvm_add_test(gpu_seek_exact_landing_hevc
        COMMAND mvm_test_gpu_decode seek "${MVM_BENCH_DIR}/v1080p60_hevc.mp4"
                299,1799,300,1800,2999,3599
        PROPERTIES REQUIRED_FILES "${MVM_BENCH_DIR}/v1080p60_hevc.mp4" TIMEOUT 300)

    mvm_add_test(gpu_marker_exact_landing_hevc
        COMMAND mvm_test_gpu_decode marker "${MVM_BENCH_DIR}/v1080p60_hevc.mp4"
                0,1,137,299,600,1799,3599
        PROPERTIES REQUIRED_FILES "${MVM_BENCH_DIR}/v1080p60_hevc.mp4" TIMEOUT 300)
endif()

# exact target search の product contract。GOP=60 の直前/境界/直後、
# repeated/unrelated seek、EOS、flush/generation、保持中 hardware frame を検査する。
if(MVM_HAVE_P2_BENCHMARK_FIXTURES)
    foreach(_seek_contract_asset v1080p60_h264 v1080p60_hevc)
        mvm_add_test(gpu_seek_exact_search_contract_${_seek_contract_asset}
            COMMAND mvm_test_gpu_decode seek-contract
                    "${MVM_BENCH_DIR}/${_seek_contract_asset}.mp4"
                    0,1,58,59,60,61,118,119,120,121,299,300,3599 4
            PROPERTIES REQUIRED_FILES "${MVM_BENCH_DIR}/${_seek_contract_asset}.mp4"
                RESOURCE_LOCK mvm_gpu RUN_SERIAL TRUE TIMEOUT 300)
    endforeach()
endif()

# 4K は診断のみ。判定には使わないが、経路が通ることは見る。
mvm_add_test(gpu_marker_4k_h264_diagnostic
    COMMAND mvm_test_gpu_decode marker "${MVM_SMOKE_DIR}/v4k60_h264.mp4" 0,137
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v4k60_h264.mp4" TIMEOUT 180)

# 10bit (P010) の経路。表示品質は判定対象外だが、
# 「NV12 決め打ちで落ちる」ことは検出しておく。
mvm_add_test(gpu_marker_4k_hevc10_diagnostic
    COMMAND mvm_test_gpu_decode marker "${MVM_SMOKE_DIR}/v4k60_hevc10.mp4" 0,137
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v4k60_hevc10.mp4" TIMEOUT 180)

# 日本語パス (UTF-8) で開けること。
mvm_add_test(gpu_decode_jp_path
    COMMAND mvm_test_gpu_decode decode "${MVM_JP_DIR}/講義映像　第1回＆演習.mp4" 3
    PROPERTIES REQUIRED_FILES "${MVM_JP_DIR}/講義映像　第1回＆演習.mp4" TIMEOUT 120)

# negative: BindFlags 検査が効いていること。
# これが落ちなければ、D3D11_BIND_SHADER_RESOURCE を外しても気づけない。
mvm_add_test(gpu_bind_flags_check_works
    COMMAND mvm_test_gpu_decode no-shader-bind
    PROPERTIES TIMEOUT 120)

# negative: hardware decoder が無い codec を成功扱いしない (exit 6)。
# PNG には D3D11VA の hwaccel が無い。software decode へは落ちない。
mvm_add_gpu_exit_test(gpu_missing_hw_decoder_negative EXIT 6
    ARGS decode "${MVM_SMOKE_DIR}/png_alpha.png"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/png_alpha.png" TIMEOUT 120)

# negative: 破損・退化素材を成功扱いしない (exit 4)。
# 「開けない」(4) と「hardware decoder が無い」(6) を区別できることが要点。
foreach(corrupt zero random truncated text subtitle_only)
    mvm_add_gpu_exit_test(gpu_invalid_media_${corrupt}_negative EXIT 4
        ARGS decode "${MVM_CORRUPT_DIR}/${corrupt}.mp4"
        PROPERTIES REQUIRED_FILES "${MVM_CORRUPT_DIR}/${corrupt}.mp4" TIMEOUT 120)
endforeach()

# negative: 範囲外のフレームへの seek を成功扱いしない (exit 3)。
mvm_add_gpu_exit_test(gpu_seek_out_of_range_negative EXIT 3
    ARGS seek "${MVM_SMOKE_DIR}/v1080p60_h264.mp4" 999999
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4" TIMEOUT 120)

# negative: 使い方の誤りを 2 で返す (実行結果の不一致 3 と区別する)。
mvm_add_gpu_exit_test(gpu_usage_error_negative EXIT 2 ARGS mvm_no_such_command)

# --- P1.1 §2: decoder snapshot の同期契約 -----------------------------------
# TSAN は Windows / MSYS2 で使えないので、決定論的な回数だけ
# 別スレッドから snapshot() を叩き、返り値の内部整合を検査する。
mvm_add_test(gpu_snapshot_race
    COMMAND mvm_test_gpu_decode snapshot-race "${MVM_SMOKE_DIR}/v1080p60_h264.mp4" 3000
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4" TIMEOUT 300)

# --- P1.1 §4: open/close soak ------------------------------------------------
# SRV cache が decoder epoch を跨いで増え続けないこと。
# **通常 CTest では 20 cycle。** 正式な 100 cycle は performance ラベル側で回す
# (100 cycle は 1 分近くかかるため)。
mvm_add_test(gpu_open_close_soak_short
    COMMAND mvm_test_gpu_decode soak "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
            "${MVM_SMOKE_DIR}/v1080p60_hevc.mp4" 20
    PROPERTIES
        REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
        TIMEOUT 600)

mvm_add_test(gpu_open_close_soak_100
    COMMAND mvm_test_gpu_decode soak "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
            "${MVM_SMOKE_DIR}/v1080p60_hevc.mp4" 100
    PROPERTIES
        REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
        LABELS performance
        TIMEOUT 1800)

# --- P1.1 §6: color correctness ---------------------------------------------
# **marker 一致を色の証拠にしない。** 既知の YUV patch を
# 表示と同じ shader で RGB 化し、期待値 (標準式から独立に計算) と照合する。
set(MVM_COLOR_DIR "${CMAKE_SOURCE_DIR}/tests/assets/color")
set(MVM_COLOR_MANIFEST "${MVM_COLOR_DIR}/manifest.txt")
set(MVM_MAKE_COLOR_HINT "pwsh scripts/make-color-fixtures.ps1")

# **fixture が無ければ configure を失敗させる (P1.2 §5)。**
#
# P1.1 は「無ければ登録しない」だった。これは最悪の形の縮退である:
#   - clean checkout では color test が 1 件も登録されない
#   - CTest は「全部通った」と報告する
#   - 色の検査が消えたことに誰も気づかない
#
# fixture は tests/assets/color/ に **追跡対象として**入れてある
# (.gitignore に例外を書いた)。それでも無いなら、生成方法を案内して止める。
if(NOT EXISTS "${MVM_COLOR_MANIFEST}")
    message(FATAL_ERROR
        "\n"
        "[mvm] color fixture がありません: ${MVM_COLOR_MANIFEST}\n"
        "\n"
        "  color correctness の検査を黙って飛ばすことはしません。\n"
        "  fixture は本来リポジトリに含まれています。失われている場合は:\n"
        "\n"
        "    ${MVM_MAKE_COLOR_HINT}\n")
endif()

# 判定対象と診断対象。**この一覧が減ったら configure が落ちる** (下の件数検査)。
set(MVM_COLOR_GATE_IDS bt709_limited bt709_full bt601_limited)
set(MVM_COLOR_DIAG_IDS bt2020ncl_limited p010_bt709_limited)
set(MVM_COLOR_EXPECTED_COUNT 5)

set(_mvm_color_registered 0)
foreach(color_id ${MVM_COLOR_GATE_IDS})
    if(NOT EXISTS "${MVM_COLOR_DIR}/${color_id}.mp4")
        message(FATAL_ERROR
            "[mvm] color fixture の素材がありません: ${MVM_COLOR_DIR}/${color_id}.mp4\n"
            "  ${MVM_MAKE_COLOR_HINT} を実行してください。")
    endif()
    mvm_add_test(gpu_color_${color_id}
        COMMAND mvm_test_gpu_decode color "${MVM_COLOR_MANIFEST}" ${color_id}
        PROPERTIES REQUIRED_FILES "${MVM_COLOR_DIR}/${color_id}.mp4" TIMEOUT 180)
    math(EXPR _mvm_color_registered "${_mvm_color_registered} + 1")
endforeach()

foreach(color_id ${MVM_COLOR_DIAG_IDS})
    if(NOT EXISTS "${MVM_COLOR_DIR}/${color_id}.mp4")
        message(FATAL_ERROR
            "[mvm] color fixture の素材がありません: ${MVM_COLOR_DIR}/${color_id}.mp4\n"
            "  ${MVM_MAKE_COLOR_HINT} を実行してください。")
    endif()
    mvm_add_test(gpu_color_${color_id}_diagnostic
        COMMAND mvm_test_gpu_decode color "${MVM_COLOR_MANIFEST}" ${color_id}
        PROPERTIES REQUIRED_FILES "${MVM_COLOR_DIR}/${color_id}.mp4" TIMEOUT 180)
    math(EXPR _mvm_color_registered "${_mvm_color_registered} + 1")
endforeach()

# **登録件数を数える。** Phase 0 の「対象 0 件のテスト群が全部通ったと
# 報告される」罠と同じ形なので、件数そのものを検査する。
if(NOT _mvm_color_registered EQUAL MVM_COLOR_EXPECTED_COUNT)
    message(FATAL_ERROR
        "[mvm] color test の登録件数が ${_mvm_color_registered} 件です "
        "(期待 ${MVM_COLOR_EXPECTED_COUNT} 件)。\n"
        "  bt709 limited / bt709 full / bt601 limited / bt2020 診断 / P010 診断 の\n"
        "  5 件が必ず登録されている必要があります。")
endif()
message(STATUS "[mvm] color test を ${_mvm_color_registered} 件登録しました")

# negative: manifest に無い id を成功扱いしない (使い方の誤り = 2)。
mvm_add_gpu_exit_test(gpu_color_unknown_id_negative EXIT 2
    ARGS color "${MVM_COLOR_MANIFEST}" mvm_no_such_fixture
    PROPERTIES REQUIRED_FILES "${MVM_COLOR_MANIFEST}")

# --- P1.2 §4: event query backend を実際に走らせる --------------------------
# fence が使える機体でも fallback 経路を通す。
# **fallback が「必要になる状況」は再現していない** (fence 非対応機が無い)。
# 走らせているのは経路であって、切り替え条件ではない。
mvm_add_test(gpu_soak_event_query_backend
    COMMAND mvm_test_gpu_decode soak "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
            "${MVM_SMOKE_DIR}/v1080p60_hevc.mp4" 12 event_query
    PROPERTIES
        REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
        TIMEOUT 600)

# --- preview_spike (Qt / device 共有) ----------------------------------------
# **Qt と FFmpeg の device 共有を検査できるのはここだけ。**
# QRhi から device を取るには window が要るため、CLI の decode テストでは見られない。
# 通常 CTest では短時間 (warm-up 1 秒 / 計測 3 秒 / seek 20) で契約だけ見る。
if(TARGET mvm_preview_spike)
    add_test(NAME preview_spike_device_sharing_contract
        COMMAND mvm_preview_spike --measure
                --media "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
                --json "${CMAKE_CURRENT_BINARY_DIR}/p1/contract-h264.json"
                --label ctest-h264
                --warmup-ms 1000 --measure-ms 3000 --seeks 20
                --marker-frames 0,1,137,299)
    set_tests_properties(preview_spike_device_sharing_contract PROPERTIES
        ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}"
        REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
        TIMEOUT 300)

    # 出力 JSON の契約を検査する。終了コードだけでは
    # 「device が別 adapter でも 0 で終わる」ような取りこぼしが出る。
    add_test(NAME preview_spike_json_contract
        COMMAND "${MVM_PWSH}" -NoProfile -File
                "${CMAKE_SOURCE_DIR}/scripts/check-p1-contract.ps1"
                -Json "${CMAKE_CURRENT_BINARY_DIR}/p1/contract-h264.json")
    set_tests_properties(preview_spike_json_contract PROPERTIES
        DEPENDS preview_spike_device_sharing_contract
        TIMEOUT 120)

    # negative: --measure に --media が無いのは使い方の誤り (exit 2)。
    add_test(NAME preview_spike_usage_error_negative
        COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/expect-exit.ps1"
                -ExpectExit 2 -Exe "$<TARGET_FILE:mvm_preview_spike>"
                -ChildArgs "|--measure")
    set_tests_properties(preview_spike_usage_error_negative PROPERTIES
        ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}" TIMEOUT 120)

    # negative: 開けない素材を成功扱いしない (exit 3)。
    add_test(NAME preview_spike_invalid_media_negative
        COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/expect-exit.ps1"
                -ExpectExit 3 -Exe "$<TARGET_FILE:mvm_preview_spike>"
                # 先頭を空要素にしている。'--measure' で始めると PowerShell が
                # -ChildArgs の値ではなくパラメータ名として束縛しようとする
                # (tests/CMakeLists.txt の lint_producer_service_negative と同じ)。
                -ChildArgs "|--measure|--media|${MVM_CORRUPT_DIR}/random.mp4|--json|${CMAKE_CURRENT_BINARY_DIR}/p1/invalid.json|--warmup-ms|500|--measure-ms|500|--seeks|0")
    set_tests_properties(preview_spike_invalid_media_negative PROPERTIES
        ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}"
        REQUIRED_FILES "${MVM_CORRUPT_DIR}/random.mp4"
        TIMEOUT 180)

    # --- performance ラベル --------------------------------------------------
    # **このラベルの緑を P1 の合格根拠にしない。**
    # 判定は scripts/p1-matrix.ps1 の記録された数値だけで行う。
    if(EXISTS "${MVM_BENCH_DIR}/v1080p60_h264.mp4")
        add_test(NAME preview_spike_1080p60_h264_smoke
            COMMAND mvm_preview_spike --measure
                    --media "${MVM_BENCH_DIR}/v1080p60_h264.mp4"
                    --json "${CMAKE_CURRENT_BINARY_DIR}/p1/smoke-h264.json"
                    --label perf-smoke-h264
                    --warmup-ms 5000 --measure-ms 15000 --seeks 100)
        set_tests_properties(preview_spike_1080p60_h264_smoke PROPERTIES
            ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}"
            LABELS performance
            TIMEOUT 900)
    endif()
endif()

# GPU 無しで final shutdown report の formal contract を検査する。
add_test(NAME p1_shutdown_contract_timeout_negative
    COMMAND "${MVM_PWSH}" -NoProfile -File
            "${CMAKE_SOURCE_DIR}/tests/gpu_preview/test-p1-shutdown-contract.ps1")
set_tests_properties(p1_shutdown_contract_timeout_negative PROPERTIES TIMEOUT 120)

# --- CI portable suite と workstation suite の境界 -------------------------
# GitHub hosted runner には Meiryo と D3D11VA hardware device が無い。
# その環境で実行不能なテストだけを明示的に分類する。SKIP_RETURN_CODE や
# software fallback で成功に見せず、ローカルの既定 CTest では従来どおり実行する。
set(MVM_WORKSTATION_TESTS
    render_audio_a1_only
    render_audio_a2_only
    render_audio_a2_only_0db
    render_audio_mixed
    verify_audio_four_frequencies
    verify_audio_gain_minus_6db
    verify_audio_no_clipping
    ownership_soak_100
    memory_soak_diagnostic
    scrub_contract_linear_8333
    scrub_contract_linear_16667
    scrub_coalescing_occurs
    preview_scenario_schema
    preview_contract_short
    preview_invalid_real_time_exit2
    p2_dual_decode_integration
    p2_gpu_compositor_offscreen
    compositor_qt_two_layers
    compositor_qt_actual_device
    compositor_qt_no_full_frame_copy
    compositor_qt_actual_target_probe
    compositor_qt_shutdown
    compositor_qt_dual_seek_64
    compositor_qt_dual_seek_64_contract
    compositor_qt_layout_epoch
    compositor_qt_layout_epoch_contract
    compositor_qt_device_change_fail_closed
    compositor_qt_completion_fatal_fail_closed
    phase4c_probe_asyncgpucopy
    p4_c_contract_smoke
    p4_c_contract_smoke_check
    gpu_decode_v1080p60_h264
    gpu_seek_flush_v1080p60_h264
    gpu_marker_v1080p60_h264
    gpu_decode_v1080p60_hevc
    gpu_seek_flush_v1080p60_hevc
    gpu_marker_v1080p60_hevc
    gpu_seek_exact_landing_hevc
    gpu_seek_exact_search_contract_v1080p60_h264
    gpu_seek_exact_search_contract_v1080p60_hevc
    gpu_marker_exact_landing_hevc
    gpu_marker_4k_h264_diagnostic
    gpu_marker_4k_hevc10_diagnostic
    gpu_decode_jp_path
    gpu_snapshot_race
    source_decode_worker_test_barrier_stop
    gpu_open_close_soak_short
    gpu_open_close_soak_100
    gpu_color_bt709_limited
    gpu_color_bt709_full
    gpu_color_bt601_limited
    gpu_color_bt2020ncl_limited_diagnostic
    gpu_color_p010_bt709_limited_diagnostic
    gpu_soak_event_query_backend
    preview_spike_device_sharing_contract
    preview_spike_json_contract
    preview_spike_1080p60_h264_smoke)

foreach(_workstation_test IN LISTS MVM_WORKSTATION_TESTS)
    if(TEST "${_workstation_test}")
        set_property(TEST "${_workstation_test}" APPEND PROPERTY LABELS workstation)
    endif()
endforeach()

# --- lint 自身の negative test ----------------------------------------------
# 検査を書いただけでは効いている証明にならない。
# 違反を入れたら落ち、正しいコードでは落ちないことを両方確かめる。

# lint.ps1 は違反を見つけたら exit 1 で落ちる。
# 「0 以外」ではなく 1 であることを見る。pwsh 自体の起動失敗や
# スクリプトの構文エラーでも落ちるが、それは 1 ではない。
add_test(NAME lint_producer_service_negative
    COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/expect-exit.ps1"
            -ExpectExit 1 -Exe "${MVM_PWSH}"
            # 先頭を空要素にしている。'-NoProfile' で始めると PowerShell が
            # -ChildArgs の値ではなくパラメータ名として束縛しようとする。
            -ChildArgs "|-NoProfile|-File|${CMAKE_SOURCE_DIR}/scripts/lint.ps1|-Path|${CMAKE_SOURCE_DIR}/tests/lint-negative/bad")

add_test(NAME lint_producer_service_control
    COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/lint.ps1"
            -Path "${CMAKE_SOURCE_DIR}/tests/lint-negative/good")

# --- Phase 1: 層の隔離検査自身の negative test -------------------------------
# 「src/media/gpu_preview は Qt を include しない」「QRhi は src/app/preview のみ」
# という規約が、機械的に強制されていることを確かめる。
# 検査を書いただけでは効いている証明にならない。
#
# 層はパスで判定するので、リポジトリ外のフィクスチャには -AsLayer で
# 「どの層として見るか」を与える。

# layer-bad/ には Qt/QRhi 違反と CPU readback 違反の両方が入っている。
# どちらか一方でも検査が効いていなければ、このテストは緑にならない。
add_test(NAME lint_layer_isolation_negative
    COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/expect-exit.ps1"
            -ExpectExit 1 -Exe "${MVM_PWSH}"
            # 先頭の空要素は lint_producer_service_negative と同じ理由。
            -ChildArgs "|-NoProfile|-File|${CMAKE_SOURCE_DIR}/scripts/lint.ps1|-Path|${CMAKE_SOURCE_DIR}/tests/lint-negative/layer-bad|-AsLayer|gpu_preview")

# 対照群: Qt も QRhi も使っていないコードでは落ちない。
# これが落ちるなら、上の negative test は意図した箇所で落ちていない。
add_test(NAME lint_layer_isolation_control
    COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/lint.ps1"
            -Path "${CMAKE_SOURCE_DIR}/tests/lint-negative/layer-good" -AsLayer gpu_preview)

# --- workstation テストの排他制御 -------------------------------------------
# workstation ラベルは「この開発機の実 GPU / audio endpoint / display を使う」
# という意味である。並列実行 (ctest -j) では、これらが同時に走ると
# device を奪い合って偽の失敗になる。実測で -j8 のとき 3 回に 1 回落ちた。
#
# 直列実行では RESOURCE_LOCK は何も変えない。並列にしたときだけ、
# workstation テスト同士が 1 件ずつ走るようになる。
#
# 個々の add_test へ手で書かず、ラベルから機械的に付ける。手書きすると
# 新しい workstation テストを足したときに付け忘れ、並列で偶発的に落ちる。
get_property(_mvm_all_tests DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY TESTS)
set(_mvm_workstation_exclusive 0)
foreach(_mvm_test IN LISTS _mvm_all_tests)
    get_test_property(${_mvm_test} LABELS _mvm_test_labels)
    if(_mvm_test_labels AND "workstation" IN_LIST _mvm_test_labels)
        set_tests_properties(${_mvm_test} PROPERTIES RESOURCE_LOCK mvm_workstation)
        math(EXPR _mvm_workstation_exclusive "${_mvm_workstation_exclusive} + 1")
    endif()
endforeach()

# 0 件なら「ラベルの付け方が変わったのに誰も気づいていない」状態である。
# 並列実行が黙って危険になるため、configure を失敗させる。
if(_mvm_workstation_exclusive EQUAL 0)
    message(FATAL_ERROR
        "[mvm] workstation ラベルのテストが 0 件です。RESOURCE_LOCK を付けられません")
endif()
message(STATUS
    "[mvm] workstation テスト ${_mvm_workstation_exclusive} 件へ RESOURCE_LOCK を設定しました")
