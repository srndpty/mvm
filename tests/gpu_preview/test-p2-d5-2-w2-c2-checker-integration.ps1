[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$C13Fixture,
    [Parameter(Mandatory=$true)][string]$C13Core,
    [Parameter(Mandatory=$true)][string]$C1Checker,
    [Parameter(Mandatory=$true)][string]$Builder,
    [Parameter(Mandatory=$true)][string]$C2Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
$caseDirectory=Join-Path $Directory "process-$PID"
if(-not(Test-Path -LiteralPath $caseDirectory)){New-Item -ItemType Directory -Path $caseDirectory|Out-Null}
& $C13Fixture -Case Good -Core $C13Core -Checker $C1Checker -Directory $caseDirectory *> $null
$c1Path=Join-Path $caseDirectory 'c13-good.json'
$c1=Get-Content -LiteralPath $c1Path -Raw -Encoding utf8|ConvertFrom-Json
$appPath=Join-Path $c1.source_c011_directory 'run-1\traced-app.json'
$app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
$app|Add-Member -NotePropertyName required_measurement_frame_count -NotePropertyValue 1
$app.presentation_opportunity.physical_vblank_domain_shadow|Add-Member -NotePropertyName required_intent_count -NotePropertyValue 1
$app|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $appPath -Encoding utf8
$c1.runs[0].sealed_input_sha256.traced_app=(Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash.ToLowerInvariant()
$c1|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $c1Path -Encoding utf8
$c2Path=Join-Path $caseDirectory 'c2-good.json'
& $Builder -C1Proof $c1Path -Output $c2Path -C1Checker $C1Checker *> $null
& $C2Checker -Proof $c2Path -C1Checker $C1Checker *> $null
$c2=Get-Content -LiteralPath $c2Path -Raw -Encoding utf8|ConvertFrom-Json
if(-not[bool]$c2.ledger_exact-or[int]$c2.formal_presented_event_count-ne1-or
   [int]$c2.satisfied_intent_count-ne1-or[int]$c2.in_domain_presented_foreign_intent_count-ne0-or
   [int]$c2.filled_physical_opportunity_count-ne1){throw 'C2 integration identityが不正です'}
Write-Output 'W2-C2 checker integration: PASS'
