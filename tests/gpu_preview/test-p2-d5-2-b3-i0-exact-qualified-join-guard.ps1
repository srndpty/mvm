[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativeReceiptAbiRemoved','NegativeReceiptSerialDerived',
        'NegativeReceiptExportRemoved','NegativeReceiptNotOneShot',
        'NegativeHookTakeReceiptRemoved','NegativeExactRecordLookupRemoved',
        'NegativeCommitOrderBypassesRecord','NegativeReservationIdentityNotScheduler',
        'NegativeExpectedSerialSequential','NegativeLatestRecordAuthority',
        'NegativeSequentialSerialEstimate','NegativeQpcProximityJoin')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Contract,
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# B3-I0 architecture guard の mutation test。
#
# guard は「expected_present_serial の authority が actual native Present record である」
# ことを source 上で固定するだけなので、実装構造が変わると静かに空振りしうる。
# guard 自身が provenance の破れを検出できることを、実 source の変異コピーで固定する。
# product code は変更しない。検査対象は guard 側である。

$relatives=@(
    'src/app/preview/native_present_hook_abi.h',
    'src/app/preview/native_present_hook.cpp',
    'src/app/preview/compositor_rhi_item.cpp',
    'src/media/gpu_preview/presentation_opportunity_scheduler.cpp',
    'src/media/gpu_preview/required_intent_queue.cpp',
    'src/media/gpu_preview/qualified_present_commit_join.cpp',
    'qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch')
# S2-h: PID だけでは isolation key にならない。Windows は PID を再利用するため、
# 過去 run の process-<PID> directory と衝突して「既存artifactを上書きしません」で
# 失敗する。S2-f2 と同じく invocation ごとに一意な suffix を付ける。

$root=Join-Path $Directory ("process-$PID-" + [guid]::NewGuid().ToString('N').Substring(0,12))
if(Test-Path -LiteralPath $root){Remove-Item -LiteralPath $root -Recurse -Force}
$sources=@{}
foreach($relative in $relatives){
    $sources[$relative]=Get-Content -LiteralPath (Join-Path $SourceRoot $relative) -Raw -Encoding utf8
}

# 変異は ASCII の識別子だけを足がかりにする。source の日本語コメントに依存しない。
function Edit-Source([string]$Relative,[string]$From,[string]$To){
    $text=$sources[$Relative]
    if($text -notmatch [regex]::Escape($From)){throw "変異対象がありません: $Relative / $From"}
    $sources[$Relative]=$text.Replace($From,$To)
}

switch($Case){
    'Good'{}
    'NegativeReceiptAbiRemoved'{
        # frameSwapped receipt を ABI から落とす。app 側で再構成する余地が戻る。
        Edit-Source 'src/app/preview/native_present_hook_abi.h' `
            'MvmNativePresentFrameSwappedReceipt' 'MvmNativePresentSwapEstimate'
    }
    'NegativeReceiptSerialDerived'{
        # receipt の present serial を record ではなく global counter から作る。
        Edit-Source 'qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch' `
            'mvmFrameSwappedReceipt.presentSerial = capture.record->presentSerial;' `
            'mvmFrameSwappedReceipt.presentSerial = static_cast<std::uint64_t>(mvmPresentSerial);'
    }
    'NegativeReceiptExportRemoved'{
        # one-shot receipt の export を消す。app は latest record へ退行するしかない。
        Edit-Source 'qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch' `
            'mvm_qt_d3d11_present_hook_take_frame_swapped_receipt' `
            'mvm_qt_d3d11_present_hook_peek_latest_present'
    }
    'NegativeReceiptNotOneShot'{
        # receipt を consume しない。1件の Present を複数の swap へ結合できてしまう。
        Edit-Source 'qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch' `
            'mvmFrameSwappedReceiptValid = false;' `
            'mvmFrameSwappedReceiptValid = true;'
    }
    'NegativeHookTakeReceiptRemoved'{
        # app 側の receipt 取得 API を消す。
        Edit-Source 'src/app/preview/native_present_hook.cpp' `
            'takeFrameSwappedReceipt' 'takeSwapEstimate'
    }
    'NegativeExactRecordLookupRemoved'{
        # present serial による exact record lookup を消す。
        Edit-Source 'src/app/preview/native_present_hook.cpp' `
            'recordForPresentSerial' 'recordForLatestPresent'
    }
    'NegativeCommitOrderBypassesRecord'{
        # frameSwapped commit を receipt -> record -> join の順から外す。
        Edit-Source 'src/app/preview/compositor_rhi_item.cpp' `
            'recordForPresentSerial' 'recordForLatestPresent'
    }
    'NegativeReservationIdentityNotScheduler'{
        # reservation identity の producer を scheduler 以外へ移す。
        Edit-Source 'src/media/gpu_preview/required_intent_queue.cpp' `
            'activeReservation_ = {++reservationSerial_,' `
            'activeReservation_ = {1,'
    }
    'NegativeExpectedSerialSequential'{
        # expected_present_serial を last + 1 で推定する。
        Edit-Source 'src/media/gpu_preview/qualified_present_commit_join.cpp' `
            'expectedPresentSerial_ = evidence.presentSerial;' `
            'expectedPresentSerial_ = expectedPresentSerial_ + 1;'
    }
    'NegativeLatestRecordAuthority'{
        # latest record を join の authority にする。
        Edit-Source 'src/app/preview/compositor_rhi_item.cpp' `
            '            const bool nativeBound =' `
            "            (void)(hook ? hook->latestSwapchainIdentity() : 0);`n            const bool nativeBound ="
    }
    'NegativeSequentialSerialEstimate'{
        # present serial を sequential に推定する。
        Edit-Source 'src/app/preview/compositor_rhi_item.cpp' `
            '            const bool nativeBound =' `
            "            (void)(receipt.presentSerial + 1);`n            const bool nativeBound ="
    }
    'NegativeQpcProximityJoin'{
        # QPC/cadence の近接で join する。
        Edit-Source 'src/app/preview/compositor_rhi_item.cpp' `
            '            const bool nativeBound =' `
            "            const auto nearestPresentQpc = gpu::qpcTicks();`n            (void)nearestPresentQpc;`n            const bool nativeBound ="
    }
}

foreach($relative in $relatives){
    $target=Join-Path $root $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force|Out-Null
    Set-Content -LiteralPath $target -Value $sources[$relative] -Encoding utf8 -NoNewline
}

$failed=$false
try{& $Contract -RepoRoot $root *> $null}catch{$failed=$true}
if($Case-eq'Good'){
    if($failed){throw '未変異のsourceをB3-I0 guardが拒否しました'}
    Write-Output "P2-D5-2 B3-I0 join guard $Case`: PASS";exit 0
}
if(-not$failed){throw "$Case をB3-I0 guardが検出できませんでした (guardが空振りしています)"}
Write-Output "P2-D5-2 B3-I0 join guard $Case`: PASS"
