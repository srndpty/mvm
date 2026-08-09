[CmdletBinding()]
param([Parameter(Mandatory)][string]$Case,[Parameter(Mandatory)][string]$Harness,[Parameter(Mandatory)][string]$OutputDir,[Parameter(Mandatory)][string]$GitExe)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
$harnessArgs=@('-DryRun','-OutputDir',(Join-Path $OutputDir $Case),'-GitExe',$GitExe)
$expected=0
switch($Case){'GoodDryRun'{}'OneRunFailure'{$harnessArgs+=@('-DryRunFailRun','2');$expected=3}'DirtyBaseline'{$harnessArgs+='-DryRunDirtyBaseline';$expected=3}'ProvenanceChange'{$harnessArgs+='-DryRunProvenanceChange';$expected=3}default{throw "未知case: $Case"}}
& (Get-Process -Id $PID).Path -NoProfile -File $Harness @harnessArgs;$actual=$LASTEXITCODE
if($actual-ne$expected){Write-Error "matrix exitが違います: case=$Case actual=$actual expected=$expected";exit 1}
if($Case-eq'GoodDryRun'){$summary=Get-Content -Raw -LiteralPath (Join-Path $OutputDir "$Case/summary.json")|ConvertFrom-Json;if($summary.formal_verdict-ne'NOT_RUN'-or$summary.dry_run_harness_pass-ne$true-or$summary.runs.Count-ne3){Write-Error 'dry-run summary契約が違います';exit 1}}
Write-Host "[p4-matrix-test] $Case expected exit $expected を確認しました"
