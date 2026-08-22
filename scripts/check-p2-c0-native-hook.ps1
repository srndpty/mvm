[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Json,
    [Parameter(Mandatory=$true)][ValidateSet('off','on')][string]$HookMode,
    [int]$ProcessExitCode=0,
    [string]$VBlankChecker=(Join-Path $PSScriptRoot 'check-p2-vblank-shadow.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Equal($Actual,$Expected,[string]$Name){if($Actual-ne$Expected){Fail "$Name が一致しません (expected=$Expected actual=$Actual)"}}
if($ProcessExitCode-ne0){Fail "application processが失敗しました: $ProcessExitCode"}
& pwsh -NoProfile -File $VBlankChecker -Json $Json -ProcessExitCode 0 *> $null
if($LASTEXITCODE-ne0){Fail 'physical VBlank authorityが不成立です'}
$raw=Get-Content -LiteralPath $Json -Raw -Encoding utf8|ConvertFrom-Json
Equal ([int]$raw.process_exit_code) 0 'raw process exit'
$hook=$raw.native_present_hook
Equal ([string]$hook.requested_mode) $HookMode 'hook mode'
Equal ([bool]$hook.available) $true 'hook availability'
Equal ([bool]$hook.shadow_only) $true 'shadow_only'
Equal ([bool]$hook.formal_counter_authority_changed) $false 'formal authority'
Equal ([string]$hook.qt_upstream_tag) 'v6.11.1' 'Qt tag'
Equal ([string]$hook.qt_upstream_commit) '59c81a3c2247b821b9b84b4eb8d939b77e07e276' 'Qt commit'
foreach($field in @('overflow_count','missing_token_count','duplicate_token_count','stale_token_count',
                     'token_set_failure_count','failed_present_count')){
    Equal ([int64]$hook.$field) 0 $field
}
$records=@($hook.records)
Equal $records.Count ([int]$hook.record_count) 'native record count'
if($HookMode-eq'off'){
    Equal ([bool]$hook.hook_enabled) $false 'OFF hook_enabled'
    Equal ([bool]$hook.capture_started) $false 'OFF capture_started'
    Equal ([bool]$hook.capture_stopped) $false 'OFF capture_stopped'
    Equal $records.Count 0 'OFF native record count'
    Write-Host 'F3-C0 native hook OFF contract: PASS'
    exit 0
}
Equal ([bool]$hook.hook_enabled) $true 'ON hook_enabled'
Equal ([bool]$hook.capture_started) $true 'ON capture_started'
Equal ([bool]$hook.capture_stopped) $true 'ON capture_stopped'
Equal ([bool]$hook.authority_pass) $true 'ON authority_pass'
Equal ([bool]$hook.authority_failure) $false 'ON authority_failure'
$swaps=@($raw.presentation_opportunity.swap_records)
Equal $records.Count $swaps.Count 'successful Present/swap count'
if($records.Count-lt2){Fail 'native Present recordが不足しています'}
$threads=@{};$swapchains=@{};$tokenSerials=@{}
for($index=0;$index-lt$records.Count;++$index){
    $record=$records[$index]
    $serial=[uint64]$record.present_serial;$tokenSerial=[uint64]$record.composition_token.token_serial
    if($serial-eq0-or($index-gt0-and$serial-ne([uint64]$records[$index-1].present_serial+1))){Fail "Present serialがstrict +1ではありません: $index"}
    if($tokenSerial-eq0-or$tokenSerials.ContainsKey($tokenSerial)){Fail "composition token serialが一意ではありません: $index"}
    $tokenSerials[$tokenSerial]=$true
    if($index-gt0-and$tokenSerial-ne([uint64]$records[$index-1].composition_token.token_serial+1)){Fail "composition token serialがstrict +1ではありません: $index"}
    Equal ([bool]$record.token_present) $true "token_present[$index]"
    Equal ([int64]$record.hresult) 0 "HRESULT[$index]"
    Equal ([int64]$record.sync_interval) 1 "SyncInterval[$index]"
    Equal ([int64]$record.present_flags) 0 "PresentFlags[$index]"
    if([int64]$record.present_enter_qpc-le0-or[int64]$record.present_return_qpc-lt[int64]$record.present_enter_qpc){Fail "Present QPC intervalが不正です: $index"}
    if($index-gt0-and[int64]$record.present_enter_qpc-le[int64]$records[$index-1].present_enter_qpc){Fail "Present enter QPCがstrictではありません: $index"}
    Equal ([int64]$record.composition_token.output_frame) ([int64]$swaps[$index].presented_output_frame) "output identity[$index]"
    Equal ([int]$record.composition_token.source_count) 2 "source count[$index]"
    $sources=@($record.composition_token.sources)
    Equal $sources.Count 2 "source array count[$index]"
    Equal ([uint64]$sources[0].source_id) 1 "source A id[$index]"
    Equal ([uint64]$sources[1].source_id) 2 "source B id[$index]"
    foreach($source in $sources){
        if([uint64]$source.source_generation-eq0-or[uint64]$source.resource_epoch-eq0){Fail "source generation/resource epochが0です: $index"}
        Equal ([int64]$source.frame_number) ([int64]$record.composition_token.output_frame) "source frame[$index]"
    }
    $threads[[string]$record.thread_id]=$true;$swapchains[[string]$record.swapchain_identity]=$true
}
Equal $threads.Count 1 'Present thread identity count'
Equal $swapchains.Count 1 'swapchain identity count'
Write-Host "F3-C0 native hook ON contract: PASS records=$($records.Count)"
