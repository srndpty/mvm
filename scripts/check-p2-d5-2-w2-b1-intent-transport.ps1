[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Json,
    [Parameter(Mandatory=$true)][string]$SourceRoot
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$sourceDirectory=$SourceRoot
function Fail([string]$Message){throw $Message}
function Equal($Actual,$Expected,[string]$Name){
    if($Actual-ne$Expected){Fail "$Name が一致しません (expected=$Expected actual=$Actual)"}
}
function U64($Value,[string]$Name){
    $parsed=[uint64]0
    if(-not[uint64]::TryParse([string]$Value,[ref]$parsed)){Fail "$Name がuint64ではありません"}
    return $parsed
}
function Read-Source([string]$Relative){
    $path=Join-Path $sourceDirectory $Relative
    if(-not(Test-Path -LiteralPath $path)){Fail "契約対象sourceがありません: $path"}
    return Get-Content -LiteralPath $path -Raw -Encoding utf8
}

$abi=Read-Source 'src/app/preview/native_present_hook_abi.h'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'
$controller=Read-Source 'apps/compositor_spike/compositor_spike_controller.cpp'
$qtPatch=Read-Source 'qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch'
if($abi-notmatch'MVM_NATIVE_PRESENT_HOOK_ABI_VERSION\s*=\s*4'){Fail 'app ABIがv4ではありません'}
foreach($type in @('MvmNativePresentCompositionToken','MvmNativePresentRecord')){
    $start=$abi.IndexOf("struct $type")
    if($start-lt0){Fail "$type がありません"}
    $body=$abi.Substring($start,[Math]::Min(1000,$abi.Length-$start))
    if($body-notmatch'std::uint64_t\s+intentOrdinal'){Fail "$type にintentOrdinalがありません"}
    if($body-notmatch'std::uint32_t\s+intentOrdinalValid'){Fail "$type にintentOrdinalValidがありません"}
}
$producerCalls=[regex]::Matches($renderer,'\.setFormalIntentOrdinal\s*\(([^)]*)\)')
Equal $producerCalls.Count 1 'formal intent producer callsite総数'
Equal (($producerCalls[0].Groups[1].Value)-replace'\s','') 'formalDecision.opportunityOrdinal' 'formal intent producer argument'
foreach($forbidden in @('outputFrameNumber','frameNumber','callbackBegin','presentSerial','qpc')){
    if($renderer-match("intentOrdinal\s*=\s*[^;]*"+[regex]::Escape($forbidden))){
        Fail "intentOrdinalを$forbidden から復元しています"
    }
}
Equal ([regex]::Matches($qtPatch,'mvmPendingToken\s*=\s*\*token').Count) 1 'app tokenからQt pending tokenへのPOD copy数'
Equal ([regex]::Matches($qtPatch,'record->token\s*=\s*mvmPendingToken').Count) 1 'Qt pending tokenからnative recordへのPOD copy数'
if($qtPatch-match'(mvmPendingToken|record->token)\.tokenSerial\s*='){Fail 'Qt hookがtoken serialを個別に改変しています'}
if($qtPatch-notmatch'record->intentOrdinal\s*=\s*mvmPendingToken\.intentOrdinal'){Fail 'Qt hookにordinal exact copyがありません'}
if($qtPatch-notmatch'record->intentOrdinalValid\s*=\s*mvmPendingToken\.intentOrdinalValid'){Fail 'Qt hookにvalidity exact copyがありません'}
if($qtPatch-notmatch'mvmNativePresentHookLayoutCompatible'){Fail 'Qt beginがlayoutをhard rejectしません'}
if($abi-notmatch'mvmNativePresentHookLayoutSignature'){Fail 'ABI layout signatureがありません'}
Equal ([regex]::Matches($controller,'formalOpportunitySchedulerEnabled\.store\s*\(').Count) 1 'formal mode設定回数'

$raw=Get-Content -LiteralPath $Json -Raw -Encoding utf8|ConvertFrom-Json
$hook=$raw.native_present_hook
$transport=$hook.intent_identity_transport
Equal ([string]$transport.schema) 'mvm-p2-d5-2-w2-b1-intent-identity-transport-2' 'transport schema'
Equal ([int]$hook.abi_version) 4 'hook ABI'
Equal ([int]$transport.abi_version) 4 'transport ABI'
Equal ([int]$transport.app_abi_version) 4 'app ABI'
Equal ([int]$transport.qt_abi_version_observed) 4 'observed Qt ABI'
Equal ([int]$hook.qt_abi_version_observed) 4 'hook observed Qt ABI'
Equal ([bool]$transport.layout_handshake_accepted) $true 'transport layout handshake'
Equal ([bool]$hook.layout_handshake_accepted) $true 'hook layout handshake'
Equal ([int]$transport.composition_token_size) 120 'composition token ABI size'
Equal ([int]$transport.native_present_record_size) 200 'native Present record ABI size'
Equal ([int]$hook.composition_token_size) 120 'hook composition token ABI size'
Equal ([int]$hook.native_present_record_size) 200 'hook native Present record ABI size'
$layoutSignature=U64 $transport.layout_signature 'transport layout signature'
if($layoutSignature-eq0){Fail 'layout signatureが0です'}
Equal (U64 $hook.layout_signature 'hook layout signature') $layoutSignature 'layout signature provenance'
Equal ([bool]$hook.available) $true 'hook availability'
Equal ([bool]$hook.hook_enabled) $true 'hook enabled'
Equal ([bool]$hook.capture_started) $true 'capture started'
Equal ([bool]$hook.capture_stopped) $true 'capture stopped'
Equal ([bool]$hook.authority_failure) $false 'hook authority failure'
foreach($field in @('overflow_count','missing_token_count','duplicate_token_count','stale_token_count',
                     'token_set_failure_count','failed_present_count')){
    Equal ([int64]$hook.$field) 0 $field
}
Equal ([bool]$transport.shadow_only) $true 'shadow_only'
Equal ([bool]$transport.performance_accounting_connected) $false 'performance accounting connection'
$records=@($transport.records)
Equal $records.Count ([int]$transport.record_count) 'ledger record count'
if($records.Count-eq0){Fail 'intent identity ledgerが空です'}
$formal=[bool]$transport.formal_mode
$presentSerials=@{};$tokenSerials=@{}
for($index=0;$index-lt$records.Count;++$index){
    $record=$records[$index]
    $presentSerial=U64 $record.native_present_serial "native present serial[$index]"
    $tokenSerial=U64 $record.native_present_embedded_token_serial "native embedded token serial[$index]"
    if($presentSerial-eq0-or$presentSerials.ContainsKey($presentSerial)){Fail "native Present serialが一意ではありません: $index"}
    if($tokenSerial-eq0-or$tokenSerials.ContainsKey($tokenSerial)){Fail "composition token serialが一意ではありません: $index"}
    $presentSerials[$presentSerial]=$true;$tokenSerials[$tokenSerial]=$true
    $tokenValid=[bool]$record.composition_token_intent_valid
    $nativeValid=[bool]$record.native_present_intent_valid
    Equal $nativeValid $tokenValid "intent validity exact copy[$index]"
    Equal (U64 $record.native_present_intent_ordinal "native intent ordinal[$index]") `
          (U64 $record.composition_token_intent_ordinal "token intent ordinal[$index]") `
          "intent ordinal exact copy[$index]"
    if($formal-and-not$tokenValid){Fail "formal tokenのintent identityがinvalidです: $index"}
    if(-not$formal-and$tokenValid){Fail "non-formal pathに架空のintent identityがあります: $index"}
}
Equal ([bool]$transport.transport_exact) $true 'transport_exact'
Equal ([string]$transport.verdict) 'INTENT_IDENTITY_ABI_V4_TRANSPORT_EXACT' 'verdict'
Write-Host "P2-D5-2-W2-B1 intent identity transport: PASS records=$($records.Count) formal=$formal"
