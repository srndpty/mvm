[CmdletBinding()]
param(
    # historical mvm sourceのcommit-ish。
    [Parameter(Mandatory=$true)][string]$HistoricalCommit,
    # current mvm sourceのcommit-ish。
    [Parameter(Mandatory=$true)][string]$CurrentCommit,
    # D1-B2 runtime reconstruction proof。historical Qt6Guiの可用性を参照する。
    [Parameter(Mandatory=$true)][string]$ReconstructionProof,
    [string]$RepositoryRoot=(Split-Path -Parent $PSScriptRoot),
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Resolve-Git(){
    # ctestのtest環境ではPATHにgitが無いことがある。既知の場所へfallbackする。
    $command=Get-Command git -CommandType Application -ErrorAction SilentlyContinue
    if($null-ne$command){return $command.Source}
    foreach($candidate in @((Join-Path $env:ProgramFiles 'Git\cmd\git.exe'),
                            (Join-Path $env:ProgramFiles 'Git\mingw64\bin\git.exe'),
                            'C:\msys64\usr\bin\git.exe')){
        if(Test-Path -LiteralPath $candidate){return $candidate}
    }
    throw 'gitを解決できません'
}
$git=Resolve-Git
function AbiVersion([string]$Commit){
    $path='src/app/preview/native_present_hook_abi.h'
    $text=& $git -C $RepositoryRoot show ("{0}:{1}" -f $Commit,$path) 2>$null
    if($LASTEXITCODE-ne0-or$null-eq$text){Fail "ABI headerを取得できません: $Commit"}
    $match=[regex]::Match(($text -join "`n"),'MVM_NATIVE_PRESENT_HOOK_ABI_VERSION\s*=\s*(\d+)')
    if(-not$match.Success){Fail "ABI versionを解析できません: $Commit"}
    return [int]$match.Groups[1].Value
}
if(-not(Test-Path -LiteralPath $ReconstructionProof)){Fail "reconstruction proofがありません: $ReconstructionProof"}
$reconstruction=Get-Content -LiteralPath $ReconstructionProof -Raw -Encoding utf8|ConvertFrom-Json
if([string]$reconstruction.schema-ne'mvm-p2-c3-a3-t2-d1b2-runtime-reconstruction-1'){Fail 'reconstruction proof schemaが不正です'}
$historicalAbi=AbiVersion $HistoricalCommit
$currentAbi=AbiVersion $CurrentCommit
$qtGui=$reconstruction.components|Where-Object component -eq 'Qt6Gui.dll'
if($null-eq$qtGui){Fail 'reconstruction proofにQt6Gui.dllがありません'}
$historicalQtAvailable=[string]$qtGui.availability-in@('EXACT_AVAILABLE','UNCHANGED_FROM_CURRENT')
# 旧exeは自身のABI versionでQt6Gui exportを検証し、patched Qtもringのabi versionを
# 検証する。両側hard rejectなのでmemory corruptionは起きないが、hookは成立しない。
$abiCompatible=$historicalAbi-eq$currentAbi
$reason=$null
$evaluable=$true
if(-not$abiCompatible-and-not$historicalQtAvailable){
    $evaluable=$false
    $reason='NATIVE_PRESENT_HOOK_ABI_MISMATCH_AND_HISTORICAL_QT_UNAVAILABLE'
}elseif(-not$abiCompatible){
    # historical Qtがあるなら旧exeと組めるが、それはexact historical runtime側の話。
    $reason='ABI_MISMATCH_REQUIRES_HISTORICAL_QT_RUNTIME'
}
$verdict=if($evaluable){'REBUILD_PROBE_EVALUABLE'}else{'REBUILD_PROBE_NOT_EVALUABLE'}
$nextAction=if($evaluable){'T2_D1_B2C_REBUILD_VS_CURRENT_PROBE'}
            else{'END_PROACTIVE_ARCHAEOLOGY_RETURN_TO_FORMAL_AUTHORITY_WIRING'}
[ordered]@{
    schema='mvm-p2-c3-a3-t2-d1b2-rebuild-evaluability-1';status='PASS';authority='diagnostic_only'
    analysis_mode='OFFLINE_SOURCE_INSPECTION_NO_NEW_ACQUISITION'
    verdict=$verdict;next_action=$nextAction;reason=$reason
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
    production_scheduler_changed=$false
    note='observerを両armへ同等に適用できない場合、rebuild probeはNOT_EVALUABLEとする。古いbuildを動かすこと自体を目的化しない。'
    historical_commit=$HistoricalCommit;current_commit=$CurrentCommit
    historical_hook_abi_version=$historicalAbi;current_hook_abi_version=$currentAbi
    hook_abi_compatible=$abiCompatible
    historical_qt_gui_availability=[string]$qtGui.availability
    historical_qt_gui_available=$historicalQtAvailable
    observer_equivalence='NATIVE_PRESENT_HOOK'
}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C3-A3-T2-D1-B2 rebuild evaluability: PASS verdict=$verdict historicalAbi=$historicalAbi currentAbi=$currentAbi"
