[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('Good','NegativeNotCaptured','NegativeClaimsAuthority','NegativeMissingSwapchainField',
                 'NegativeCapabilityContaminated','NegativeZeroIdentity')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null

$swapchain=[ordered]@{available=$true;identity='0x13dd2a6a040';width=1920;height=1080;format=28
    stereo=$false;sample_count=1;sample_quality=0;buffer_usage=32;buffer_count=2;scaling=1
    swap_effect=4;alpha_mode=0;flags=64;frame_latency_waitable_object=$true
    maximum_frame_latency_available=$true;maximum_frame_latency=2}
$adapter=[ordered]@{available=$true;luid_low=59807;luid_high=0;description='TEST GPU'}
$output=[ordered]@{available=$true;monitor_handle='0x10001';device_name='\\.\DISPLAY1'
    desktop_left=0;desktop_top=0;desktop_right=1920;desktop_bottom=1200;attached_to_desktop=$true}
$window=[ordered]@{available=$true;handle='0x8a10e0';style=2530148352;ex_style=256
    cloaked_available=$true;cloaked=0;window_left=0;window_top=0;window_right=1936
    window_bottom=1119;client_width=1920;client_height=1080}
$capability=[ordered]@{tearing_support_available=$true;tearing_supported=$true
    hardware_composition_support_available=$true;hardware_composition_support_flags=3}
$captured=$true
$authority=$false
switch($Case){
    'NegativeNotCaptured'{$captured=$false}
    'NegativeClaimsAuthority'{$authority=$true}
    'NegativeMissingSwapchainField'{$swapchain.Remove('scaling')}
    'NegativeCapabilityContaminated'{$capability['present_mode']='Hardware_Composed_Independent_Flip'}
    'NegativeZeroIdentity'{$swapchain.identity='0x0'}
}
$preflight=[ordered]@{
    schema='mvm-p2-c3-a3-t2-d1b0-eligibility-preflight-1';authority='diagnostic_only'
    is_presentation_path_authority=$authority;captured=$captured;error=''
    swapchain=$swapchain;adapter=$adapter;output=$output;window=$window;capability=$capability}
$appPath=Join-Path $Directory 'app.json'
[ordered]@{presentation_eligibility_preflight=$preflight}|ConvertTo-Json -Depth 8|
    Set-Content -LiteralPath $appPath -Encoding utf8

$proof=Join-Path $Directory 'preflight-proof.json'
$failed=$false
try{
    & pwsh -NoProfile -File $Checker -AppJson $appPath -Output $proof
    if($LASTEXITCODE-ne0){$failed=$true}
}catch{$failed=$true}
if($Case-eq'Good'){
    if($failed){throw '正のpreflight契約が失敗しました'}
    if(-not(Test-Path -LiteralPath $proof)){throw 'preflight proofが出力されていません'}
    $result=Get-Content -LiteralPath $proof -Raw -Encoding utf8|ConvertFrom-Json
    if([bool]$result.is_presentation_path_authority){throw 'proofがpresentation path authorityを主張しています'}
    Write-Host 'F3-C3-A3-T2-D1-B0 preflight contract: PASS (Good)'
    exit 0
}
if(-not$failed){throw "壊したpreflight契約が通過しました: $Case"}
Write-Host "F3-C3-A3-T2-D1-B0 preflight contract: PASS ($Case rejected)"
