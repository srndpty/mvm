[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AppJson,
    [string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Require($Object,[string]$Name){
    if($null-eq$Object-or-not($Object.PSObject.Properties.Name-contains$Name)){Fail "preflight fieldがありません: $Name"}
    return $Object.$Name
}
if(-not(Test-Path -LiteralPath $AppJson)){Fail "app jsonがありません: $AppJson"}
$app=Get-Content -LiteralPath $AppJson -Raw -Encoding utf8|ConvertFrom-Json
$preflight=Require $app 'presentation_eligibility_preflight'
if([string](Require $preflight 'schema')-ne'mvm-p2-c3-a3-t2-d1b0-eligibility-preflight-1'){Fail 'preflight schemaが不正です'}
if([string](Require $preflight 'authority')-ne'diagnostic_only'){Fail 'preflight authorityがdiagnostic_onlyではありません'}
# capabilityをpresentation pathのauthorityとして扱わないことをschema上固定する。
if([bool](Require $preflight 'is_presentation_path_authority')){Fail 'preflightがpresentation path authorityを主張しています'}
if(-not[bool](Require $preflight 'captured')){Fail "preflightが取得できていません: $([string]$preflight.error)"}
$swapchain=Require $preflight 'swapchain'
if(-not[bool](Require $swapchain 'available')){Fail 'swapchain descが取得できていません'}
foreach($field in @('identity','width','height','format','stereo','sample_count','sample_quality',
                    'buffer_usage','buffer_count','scaling','swap_effect','alpha_mode','flags',
                    'frame_latency_waitable_object','maximum_frame_latency_available',
                    'maximum_frame_latency')){
    $null=Require $swapchain $field
}
if([string]$swapchain.identity-eq'0x0'){Fail 'swapchain identityが0です'}
if([long]$swapchain.width-le0-or[long]$swapchain.height-le0){Fail 'swapchain extentが不正です'}
if([long]$swapchain.buffer_count-le0){Fail 'buffer countが不正です'}
$adapter=Require $preflight 'adapter'
if(-not[bool](Require $adapter 'available')){Fail 'adapter identityが取得できていません'}
$outputIdentity=Require $preflight 'output'
if(-not[bool](Require $outputIdentity 'available')){Fail 'output identityが取得できていません'}
foreach($field in @('monitor_handle','device_name','desktop_left','desktop_top','desktop_right',
                    'desktop_bottom','attached_to_desktop')){$null=Require $outputIdentity $field}
$window=Require $preflight 'window'
if(-not[bool](Require $window 'available')){Fail 'window stateが取得できていません'}
foreach($field in @('handle','style','ex_style','cloaked_available','cloaked','window_left',
                    'window_top','window_right','window_bottom','client_width','client_height')){
    $null=Require $window $field
}
$capability=Require $preflight 'capability'
foreach($field in @('tearing_support_available','tearing_supported',
                    'hardware_composition_support_available','hardware_composition_support_flags')){
    $null=Require $capability $field
}
# capability と observed runtime が同一objectに混ざっていないことを要求する。
foreach($forbidden in @('present_mode','independent_flip','displayed_qpc','mpo_observed')){
    if($capability.PSObject.Properties.Name-contains$forbidden){Fail "capabilityにobserved runtimeが混入しています: $forbidden"}
}
$result=[ordered]@{
    schema='mvm-p2-c3-a3-t2-d1b0-preflight-proof-1';status='PASS';authority='diagnostic_only'
    is_presentation_path_authority=$false
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
    production_scheduler_changed=$false
    swapchain=$swapchain;adapter=$adapter;output=$outputIdentity;window=$window;capability=$capability
}
if(-not[string]::IsNullOrWhiteSpace($Output)){
    $result|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $Output -Encoding utf8
}
Write-Host ("F3-C3-A3-T2-D1-B0 preflight: PASS swap_effect={0} buffer_count={1} scaling={2} alpha={3} flags={4} hwcomp={5}" -f `
    $swapchain.swap_effect,$swapchain.buffer_count,$swapchain.scaling,$swapchain.alpha_mode,$swapchain.flags,$capability.hardware_composition_support_flags)
