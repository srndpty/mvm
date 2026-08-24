[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$entrypoint=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/check-p2-d5-2-w2-c14-sealed-mapping-replay.ps1') -Raw -Encoding utf8
$checker=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/check-p2-d5-2-w2-c13-formal-population.ps1') -Raw -Encoding utf8
function Require([string]$Pattern,[string]$Message){if($checker-notmatch$Pattern){throw $Message}}
if($entrypoint-notmatch'check-p2-d5-2-w2-c13-formal-population\.ps1'){throw 'C1.4 entrypointがclosure checkerをconsumeしていません'}
Require "Get-FileHash[\s\S]+traced_app[\s\S]+present_history_raw[\s\S]+b2_terminal_shadow[\s\S]+upstream_inventory_proof" 'sealed 4-input hash replayがありません'
Require "p2-d5-2-w2-c1-mapping-core\.ps1" '既存C1 mapping coreをconsumeしていません'
Require "p2-d5-2-w2-c13-formal-population-core\.ps1" '既存C1.3 population coreをconsumeしていません'
Require "Invoke-MvmDisplayedQpcPhysicalMapping[\s\S]+replayedFormal[\s\S]+replayedObserved" 'formal/observed mapping replayがありません'
Require "physical_vblank_ordinal','physical_vblank_qpc'" 'physical ordinal/QPC照合がありません'
Require 'Compare-ReplayedMapping[\s\S]+formal[\s\S]+Compare-ReplayedMapping[\s\S]+observed' 'formal/observed artifact比較がありません'
Write-Output 'W2-C1.4 sealed mapping replay architecture: PASS'
