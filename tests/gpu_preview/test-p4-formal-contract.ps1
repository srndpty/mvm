[CmdletBinding()]
param([Parameter(Mandatory)][string]$Case,[Parameter(Mandatory)][string]$Checker,[Parameter(Mandatory)][string]$Generator,[Parameter(Mandatory)][string]$OutputDir)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
New-Item -ItemType Directory -Force -Path $OutputDir|Out-Null
$json=Join-Path $OutputDir "$Case.json"
& $Generator -Output $json -Case $Case
& $Checker -Json $json
$actual=$LASTEXITCODE;$expected=if($Case-eq'GoodFormal'){0}else{3}
if($actual-ne$expected){Write-Error "checker終了コードが違います: case=$Case actual=$actual expected=$expected";exit 1}
Write-Host "[p4-formal-test] $Case expected exit $expected を確認しました"
