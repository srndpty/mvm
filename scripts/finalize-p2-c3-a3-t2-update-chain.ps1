[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AppJson,
    [Parameter(Mandatory=$true)][ValidateSet('CONTROL','TARGET_PIXEL')][string]$ExpectedMode,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$Executable=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ucrt64-release\bin\mvm_compositor_spike.exe'),
    [string]$QtGui=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtbase-c0\bin\Qt6Gui.dll'),
    [string]$QtQuick=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtquick-t2-runtime\Qt6Quick.dll')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$repo=Split-Path -Parent $PSScriptRoot
$checker=Join-Path $PSScriptRoot 'check-p2-c3-a3-t2-update-chain.ps1'
$qtBasePatch=Join-Path $repo 'qt-patches\qtbase-6.11.1\0002-mvm-dirty-propagation-hook.patch'
$qtQuickPatch=Join-Path $repo 'qt-patches\qtdeclarative-6.11.1\0001-mvm-dirty-propagation-hook.patch'
foreach($path in @($AppJson,$Executable,$QtGui,$QtQuick,$checker,$qtBasePatch,$qtQuickPatch)){
    if(-not(Test-Path -LiteralPath $path)){throw "T2 finalize必須pathがありません: $path"}
}
if(Test-Path -LiteralPath $Output){throw "既存T2 checkpointを上書きしません: $Output"}
$proofPath=[IO.Path]::ChangeExtension($Output,'.proof.json')
if(Test-Path -LiteralPath $proofPath){throw "既存T2 proofを上書きしません: $proofPath"}
& pwsh -NoProfile -File $checker -AppJson $AppJson -ExpectedMode $ExpectedMode -Output $proofPath
if($LASTEXITCODE-ne0){throw 'T2 update chain checkerが失敗しました'}
$proof=Get-Content -LiteralPath $proofPath -Raw -Encoding utf8|ConvertFrom-Json
[ordered]@{
    schema='mvm-p2-c3-a3-t2-update-chain-checkpoint-1';status='PASS';authority='diagnostic_only'
    expected_mode=$ExpectedMode;proof=$proof
    identities=[ordered]@{
        app_json_sha256=Hash $AppJson;proof_json_sha256=Hash $proofPath
        executable_sha256=Hash $Executable;qt_gui_dll_sha256=Hash $QtGui
        qt_quick_dll_sha256=Hash $QtQuick;checker_sha256=Hash $checker
        qtbase_patch_sha256=Hash $qtBasePatch;qtdeclarative_patch_sha256=Hash $qtQuickPatch
        qtbase_upstream_commit='59c81a3c2247b821b9b84b4eb8d939b77e07e276'
        qtdeclarative_upstream_commit='a02bed441965ee1f18f856352c7d5ee5ba35d795'
    }
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
    production_scheduler_changed=$false
}|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C3-A3-T2 update chain finalize: PASS mode=$ExpectedMode ($Output)"
