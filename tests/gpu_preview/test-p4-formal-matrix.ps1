[CmdletBinding()]
param([Parameter(Mandatory)][string]$Case,[Parameter(Mandatory)][string]$Harness,[Parameter(Mandatory)][string]$OutputDir,[Parameter(Mandatory)][string]$GitExe)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
$caseOutput=Join-Path $OutputDir $Case
$harnessArgs=@('-DryRun','-OutputDir',$caseOutput,'-GitExe',$GitExe,
    '-SourceA',(Join-Path $caseOutput 'missing-source-a.mp4'),
    '-SourceB',(Join-Path $caseOutput 'missing-source-b.mp4'),
    '-Executable',(Join-Path $caseOutput 'missing-executable.exe'))
$expected=0
switch($Case){'GoodDryRun'{}'DetachedHead'{$harnessArgs+='-DryRunDetachedHead'}'OneRunFailure'{$harnessArgs+=@('-DryRunFailRun','2');$expected=3}'DirtyBaseline'{$harnessArgs+='-DryRunDirtyBaseline';$expected=3}'ProvenanceChange'{$harnessArgs+='-DryRunProvenanceChange';$expected=3}'MissingRaw'{$harnessArgs+=@('-DryRunMissingRawRun','2');$expected=3}'InvalidRuntimeBin'{$harnessArgs+=@('-DryRunRuntimePreflight','-RuntimeBin',(Join-Path $caseOutput 'missing-runtime'));$expected=3}default{throw "未知case: $Case"}}
& (Get-Process -Id $PID).Path -NoProfile -File $Harness @harnessArgs;$actual=$LASTEXITCODE
if($actual-ne$expected){Write-Error "matrix exitが違います: case=$Case actual=$actual expected=$expected";exit 1}
$summaryPath=Join-Path $caseOutput 'summary.json'
if(-not(Test-Path -LiteralPath $summaryPath)){Write-Error "summaryがありません: $summaryPath";exit 1}
$summary=Get-Content -Raw -LiteralPath $summaryPath|ConvertFrom-Json
if($summary.formal_verdict-ne'NOT_RUN'){Write-Error 'dry-runのformal_verdictがNOT_RUNではありません';exit 1}
if($Case-eq'GoodDryRun' -and ($summary.dry_run_harness_pass-ne$true-or$summary.runs.Count-ne3)){Write-Error 'dry-run summary契約が違います';exit 1}
if($Case-eq'DetachedHead' -and ($summary.dry_run_harness_pass-ne$true-or$null-ne$summary.branch-or$summary.runs.Count-ne3)){Write-Error 'detached HEAD summary契約が違います';exit 1}
if($Case-eq'MissingRaw'){$run=$summary.runs|Where-Object{$_.run-eq2};if($summary.all_runs_pass-ne$false-or$run.raw_exists-ne$false-or$run.process_exit_code-ne7-or$run.checker_ran-ne$false-or$run.checker_exit_code-ne3){Write-Error 'MissingRaw evidence契約が違います';exit 1}}
if($Case-eq'InvalidRuntimeBin' -and ($summary.all_runs_pass-ne$false-or$summary.runtime_path_preflight_pass-ne$false-or$summary.runs.Count-ne0-or$summary.failure_reason-notmatch'native runtime binがありません')){Write-Error 'InvalidRuntimeBin evidence契約が違います';exit 1}
Write-Host "[p4-matrix-test] $Case expected exit $expected を確認しました"
