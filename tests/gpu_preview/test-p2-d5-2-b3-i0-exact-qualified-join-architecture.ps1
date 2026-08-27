param([Parameter(Mandatory=$true)][string]$RepoRoot)
$ErrorActionPreference='Stop'
function Read-Source([string]$Relative){Get-Content -Raw -LiteralPath (Join-Path $RepoRoot $Relative)}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Reject([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}

$abi=Read-Source 'src/app/preview/native_present_hook_abi.h'
$hook=Read-Source 'src/app/preview/native_present_hook.cpp'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'
$scheduler=Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.cpp'
$join=Read-Source 'src/media/gpu_preview/qualified_present_commit_join.cpp'
$patch=Read-Source 'qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch'

Require $abi 'MvmNativePresentFrameSwappedReceipt' 'frameSwapped receipt ABIがありません'
Require $patch 'mvmEndPresentCapture[\s\S]+mvmFrameSwappedReceipt\.presentSerial\s*=\s*capture\.record->presentSerial' 'actual Present recordがreceipt authorityをmintしていません'
Require $patch 'mvm_qt_d3d11_present_hook_take_frame_swapped_receipt' 'one-shot receipt exportがありません'
Require $patch 'mvmFrameSwappedReceiptValid\s*=\s*false' 'receiptがone-shot consumeされません'
Require $hook 'NativePresentHook::takeFrameSwappedReceipt' 'app側receipt取得APIがありません'
Require $hook 'NativePresentHook::recordForPresentSerial' 'present serial exact record lookupがありません'
Require $renderer 'takeFrameSwappedReceipt[\s\S]+recordForPresentSerial[\s\S]+bindNativePresent[\s\S]+commitFrameSwapped' 'frameSwapped commitがreceipt→record→qualified join順ではありません'
Require $scheduler 'decision\.reservationId\s*=\s*\+\+reservationSerial_' 'primary reservation identityがscheduler由来ではありません'
Require $join 'expectedPresentSerial_\s*=\s*evidence\.presentSerial' 'expected_present_serialがactual record由来ではありません'
Reject $renderer 'latestSwapchainIdentity\(\)[\s\S]{0,500}(bindNativePresent|commitFrameSwapped)' 'qualified joinがlatest recordをauthorityにしています'
Reject $renderer 'presentSerial\s*\+\s*1|\+\+.*presentSerial' 'present serialをsequential推定しています'
Reject $renderer '(nearest|tolerance|cadence).*present' 'QPC/cadence heuristicをqualified joinへ混入しています'
