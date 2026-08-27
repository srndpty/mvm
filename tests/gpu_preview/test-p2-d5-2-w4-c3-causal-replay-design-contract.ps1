[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Contract,
    [ValidateSet('Good','NegativeCauseEnum','NegativeExactJoin','NegativeAlternativeStop','NegativeAuthority')]
    [string]$Case='Good'
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$contractText=Get-Content -LiteralPath $Contract -Raw -Encoding utf8
switch($Case){
    'NegativeCauseEnum' {$contractText=$contractText.Replace('OUTSIDE_SOURCE_DOMAIN_DECISION','OUTSIDE_REQUIRED_DOMAIN_DECISION')}
    'NegativeExactJoin' {$contractText=$contractText.Replace('nearest QPC、同一QPC、token serialの類似','nearest QPCを許可し、同一QPC、token serialの類似')}
    'NegativeAlternativeStop' {$contractText=$contractText.Replace('pre.explicit_stop_requested = false','pre.explicit_stop_requested = true')}
    'NegativeAuthority' {$contractText=$contractText.Replace('canonical performance authorityへ昇格しない','canonical performance authorityへ昇格する')}
}
function Require([string]$Pattern,[string]$Message){
    if($contractText-notmatch$Pattern){throw $Message}
}
function Deny([string]$Pattern,[string]$Message){
    if($contractText-match$Pattern){throw $Message}
}
try{
    Require 'cause:[\s\S]+DOMAIN_TERMINAL[\s\S]+PLANNED_WINDOW_END[\s\S]+EXPLICIT_STOP' 'stop cause enumが完全ではありません'
    Require 'scheduler_invocation_serial[\s\S]+OUTSIDE_SOURCE_DOMAIN_DECISION[\s\S]+PAST_SOURCE_DOMAIN' 'terminal invocationのexact serial joinがありません'
    Deny 'OUTSIDE_REQUIRED_DOMAIN_DECISION' 'source-domain resultへrequired-domain enumが混入しています'
    Require 'nearest QPC、同一QPC、token serialの類似[\s\S]+使わない' 'nearest QPC joinが禁止されていません'
    Require 'pre\.explicit_stop_requested = false[\s\S]+pre\.planned_window_end_reached = false[\s\S]+pre\.fatal_latched = false' 'alternative stopの全件排除条件がありません'
    Require 'canonical performance authorityへ昇格しない' 'C3 diagnosticがperformance authorityから分離されていません'
    Require 'W4_C3_PARTIAL[\s\S]+root_cause_determined=false' 'partial時のroot cause昇格禁止がありません'
    if($Case-ne'Good'){throw "mutationが検出されませんでした: $Case"}
}catch{
    if($Case-eq'Good'-or$_.Exception.Message-like'mutationが検出されませんでした:*'){throw}
}
Write-Output "W4-C3 causal replay design contract: PASS ($Case)"
