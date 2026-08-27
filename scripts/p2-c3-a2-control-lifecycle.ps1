[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,300)][int]$WarmupSeconds=5,
    [ValidateRange(1,300)][int]$MeasureSeconds=60,
    [ValidateRange(30,600)][int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$runner=Join-Path $PSScriptRoot 'p2-c0-native-etw.ps1'
$attribution=Join-Path $PSScriptRoot 'p2-c3-a2-lifecycle-attribution.ps1'
foreach($path in @($runner,$attribution)){if(-not(Test-Path -LiteralPath $path)){throw "F3-C3-A2必須scriptがありません: $path"}}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存F3-C3-A2 artifactを上書きしません: $OutputDirectory"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$canonical=Join-Path $OutputDirectory 'canonical';$proof=Join-Path $OutputDirectory 'attribution'
& pwsh -NoProfile -File $runner -OutputDirectory $canonical -AcquisitionMode CanonicalPresentMonLive `
    -SubmissionMode CONTROL -WarmupSeconds $WarmupSeconds -MeasureSeconds $MeasureSeconds -TimeoutSeconds $TimeoutSeconds
if($LASTEXITCODE-ne0){throw 'F3-C3-A2 CONTROL-only canonical runが失敗しました'}
& pwsh -NoProfile -File $attribution -CanonicalDirectory $canonical -OutputDirectory $proof
if($LASTEXITCODE-ne0){throw 'F3-C3-A2 lifecycle attributionが失敗しました'}
Write-Host "F3-C3-A2 CONTROL-only lifecycle run: PASS ($OutputDirectory)"
