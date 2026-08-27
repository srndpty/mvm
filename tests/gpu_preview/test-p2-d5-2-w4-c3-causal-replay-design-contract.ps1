[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Contract,
    [ValidateSet('Good','NegativeCauseEnum','NegativeExactJoin','NegativeAlternativeStop',
        'NegativeStopPublishSerial','NegativeTargetPredicateReplay','NegativeAuthority')]
    [string]$Case='Good'
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$contractText=Get-Content -LiteralPath $Contract -Raw -Encoding utf8
switch($Case){
    'NegativeCauseEnum' {$contractText=$contractText.Replace('OUTSIDE_SOURCE_DOMAIN_DECISION','OUTSIDE_REQUIRED_DOMAIN_DECISION')}
    'NegativeExactJoin' {$contractText=$contractText.Replace('nearest QPC、同一QPC、token serialの類似','nearest QPCを許可し、同一QPC、token serialの類似')}
    'NegativeAlternativeStop' {$contractText=$contractText.Replace('pre.explicit_stop_requested = false','pre.explicit_stop_requested = true')}
    'NegativeStopPublishSerial' {$contractText=$contractText.Replace('pre.explicit_stop_publish_serial == at_gate_close.explicit_stop_publish_serial','pre.explicit_stop_publish_serial <= at_gate_close.explicit_stop_publish_serial')}
    'NegativeTargetPredicateReplay' {$contractText=$contractText.Replace('past_source_domain = target >= required_frame_count','past_source_domain = target > required_frame_count')}
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
    Require 'publish siteはflag storeより前にserialをfetch_add\(1\)する' 'publication serialのwriter-side順序規約がありません'
    Require 'pre\.explicit_stop_publish_serial +== at_gate_close\.explicit_stop_publish_serial' 'explicit stop publication serialのexact一致条件がありません'
    Require 'pre\.fatal_publish_serial +== at_gate_close\.fatal_publish_serial' 'fatal publication serialのexact一致条件がありません'
    Require 'at_gate_close\.explicit_stop_publish_serial +== measurement_start\.explicit_stop_publish_serial' 'measurement開始からのexplicit stop publication排除条件がありません'
    Require 'scheduler_config:[\s\S]+source_frame_offset[\s\S]+source_fps_numerator[\s\S]+source_fps_denominator[\s\S]+refresh_numerator[\s\S]+refresh_denominator[\s\S]+required_frame_count' 'replay入力のscheduler configがありません'
    Require 'past_source_domain = target >= required_frame_count' 'source-domain predicateのexact replay式がありません'
    Require 'replayed_target_frame +== recorded target_frame' 'target replayのexact一致条件がありません'
    Require 'replayed_past_source_domain +== recorded past_source_domain' 'past_source_domain replayのexact一致条件がありません'
    Require 'terminal直前decisionのpast_source_domain = false' 'terminal直前decisionのpredicate条件がありません'
    Require 'NegativeExplicitStopPublishedBetweenPreAndGateClose' 'publication race negativeがありません'
    Require 'NegativeTerminalTargetPredicateMutation' 'terminal predicate mutation negativeがありません'
    Require 'canonical performance authorityへ昇格しない' 'C3 diagnosticがperformance authorityから分離されていません'
    Require 'W4_C3_PARTIAL[\s\S]+root_cause_determined=false' 'partial時のroot cause昇格禁止がありません'
    if($Case-ne'Good'){throw "mutationが検出されませんでした: $Case"}
}catch{
    if($Case-eq'Good'-or$_.Exception.Message-like'mutationが検出されませんでした:*'){throw}
}
Write-Output "W4-C3 causal replay design contract: PASS ($Case)"
