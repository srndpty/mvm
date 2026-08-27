[CmdletBinding()]
param([Parameter(Mandatory)][string]$Case,[Parameter(Mandatory)][string]$Checker,[Parameter(Mandatory)][string]$Generator,[Parameter(Mandatory)][string]$OutputDir)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
New-Item -ItemType Directory -Force -Path $OutputDir|Out-Null
$json=Join-Path $OutputDir "$Case.json"
& $Generator -Output $json -Case $Case
# checker は Write-Host で報告するため information stream を取り込む。
$output=& $Checker -Json $json 6>&1
$actual=$LASTEXITCODE
$output|ForEach-Object{Write-Host $_}
# P2-D5-2-W2-E: fps / drop は legacy presentation authority 由来の diagnostic へ降格した。
# canonical performance verdict は W3 の formal-v2 fresh acquisition まで出さないため、
# threshold を割った run は FAIL ではなく「診断値として記録される」ことを期待する。
$legacyPerformanceDiagnosticCases=@('FpsBelow55','DropOver2Percent')
$expected=if($Case-eq'GoodFormal'-or$Case-in$legacyPerformanceDiagnosticCases){0}else{3}
if($actual-ne$expected){Write-Error "checker終了コードが違います: case=$Case actual=$actual expected=$expected";exit 1}
if($Case-in$legacyPerformanceDiagnosticCases){
    $text=@($output)-join "`n"
    if($text-notmatch'\[W2-E\] legacy presentation metrics \(diagnostic, non-authoritative\)'){
        Write-Error "case=$Case でlegacy metricがdiagnosticとして報告されていません";exit 1
    }
    if($text-notmatch'canonical_performance_verdict=DEFERRED_TO_W3'){
        Write-Error "case=$Case でcanonical performance verdictがW3へ保留されていません";exit 1
    }
    Write-Host "[p4-formal-test] $Case legacy performance metricはdiagnosticとして記録されました"
    exit 0
}
Write-Host "[p4-formal-test] $Case expected exit $expected を確認しました"
