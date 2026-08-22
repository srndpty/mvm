[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SourceDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$Checker=(Join-Path $PSScriptRoot 'check-p2-c0-native-etw.ps1'),
    [string]$NativeChecker=(Join-Path $PSScriptRoot 'check-p2-c0-native-hook.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Fail([string]$Message){throw $Message}
if(-not(Test-Path -LiteralPath $SourceDirectory)){Fail "source artifactがありません: $SourceDirectory"}
if(Test-Path -LiteralPath $OutputDirectory){Fail "既存recheck artifactを上書きしません: $OutputDirectory"}
$SourceDirectory=(Resolve-Path -LiteralPath $SourceDirectory).Path
foreach($path in @($Checker,$NativeChecker)){if(-not(Test-Path -LiteralPath $path)){Fail "checkerがありません: $path"}}
$manifestPath=Join-Path $SourceDirectory 'manifest.sha256'
foreach($name in @('traced-app.json','present-history-raw.json','summary.json','manifest.sha256')){
    if(-not(Test-Path -LiteralPath (Join-Path $SourceDirectory $name))){Fail "source artifact fieldがありません: $name"}
}
foreach($line in Get-Content -LiteralPath $manifestPath -Encoding ascii){
    if($line-notmatch'^([0-9a-fA-F]{64})  (.+)$'){Fail "source manifest行が不正です: $line"}
    $path=Join-Path $SourceDirectory $Matches[2]
    if(-not(Test-Path -LiteralPath $path)-or(Hash $path)-ne$Matches[1].ToLowerInvariant()){
        Fail "source manifest検証に失敗しました: $($Matches[2])"
    }
}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$appPath=Join-Path $SourceDirectory 'traced-app.json'
$etwPath=Join-Path $SourceDirectory 'present-history-raw.json'
$oraclePath=Join-Path $OutputDirectory 'oracle.json'
& pwsh -NoProfile -File $Checker -AppJson $appPath -EtwJson $etwPath -Output $oraclePath `
    -NativeChecker $NativeChecker -ProcessExitCode 0
if($LASTEXITCODE-ne0){Fail "C0 native/ETW recheckが失敗しました: $LASTEXITCODE"}
$oracle=Get-Content -LiteralPath $oraclePath -Raw -Encoding utf8|ConvertFrom-Json
[ordered]@{
    schema='mvm-p2-c0-native-etw-recheck-1';source_artifact=$SourceDirectory
    source_manifest_verified=$true;recheck_status='PASS'
    measurement_domain=[string]$oracle.measurement_domain
    measurement_record_count=[int64]$oracle.native_present_count
    boundary_straddling_native_count=[int64]$oracle.boundary_straddling_native_count
    boundary_straddling_etw_count=[int64]$oracle.boundary_straddling_etw_count
    presented_count=[int64]$oracle.presented_count;discarded_count=[int64]$oracle.discarded_count
    incomplete_unknown_count=[int64]$oracle.incomplete_unknown_count;lost_count=[int64]$oracle.lost_count
    source_identities=[ordered]@{
        app_sha256=Hash $appPath;etw_sha256=Hash $etwPath;manifest_sha256=Hash $manifestPath
    }
}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $OutputDirectory 'recheck.json') -Encoding utf8
$outputManifest=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$outputManifest}|Sort-Object Name|ForEach-Object{
    "$(Hash $_.FullName)  $($_.Name)"
}|Set-Content -LiteralPath $outputManifest -Encoding ascii
Write-Host "F3-C0 native/ETW recheck: PASS ($OutputDirectory)"
