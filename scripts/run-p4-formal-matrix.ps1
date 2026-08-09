[CmdletBinding()]
param(
    [switch]$DryRun,
    [ValidateRange(0,3)][int]$DryRunFailRun = 0,
    [switch]$DryRunDirtyBaseline,
    [switch]$DryRunProvenanceChange,
    [string]$GitExe = 'git',
    [string]$OutputDir = 'tmp/p4-formal-matrix',
    [string]$Executable = 'build/ucrt64-release/bin/mvm_p4_composition_spike.exe',
    [string]$SourceA = 'tests/assets/p3_audio/p3_av_h264_aac.mp4',
    [string]$SourceB = 'tests/assets/p3_audio/p3_video_hevc_b.mp4'
)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
$gitCommand=$GitExe
$root=Split-Path -Parent $PSScriptRoot
$checker=Join-Path $PSScriptRoot 'check-p4-formal-contract.ps1'
$generator=Join-Path $PSScriptRoot 'new-p4-formal-synthetic.ps1'
$canonical='0:S0;600:S1;1200:S2;1800:S3;2400:S0;3000:S1'
$scheduleHash='5b66543f43f98ad261a5a96e961332ef4a3d5b21f8f30b1713b4ff420a855f79'
$fixtureA='d398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308'
$fixtureB='fe7cd1a45d101d363cb3930497601efd41dd55fd36c194acf8f24e3e4728b479'
function Invoke-Git([string[]]$Arguments){$v=& $gitCommand @Arguments;if($LASTEXITCODE-ne0){throw "git $($Arguments-join' ') に失敗しました"};$v}
function Hash([string]$Path){(Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()}
function Child([string]$File,[string[]]$Arguments){$p=Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList (@('-NoProfile','-File',$File)+$Arguments) -Wait -PassThru -NoNewWindow;[int]$p.ExitCode}
Push-Location $root
try{
    $actualDirty=@(Invoke-Git @('status','--porcelain'))
    $startClean=$actualDirty.Count-eq0
    $head=(Invoke-Git @('rev-parse','HEAD')).Trim();$branch=(Invoke-Git @('branch','--show-current')).Trim()
    $computed=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($canonical))).ToLowerInvariant()
    if($computed-ne$scheduleHash){throw 'formal canonical/hashの内部契約が不一致です'}
    if(-not$DryRun -and -not$startClean){throw 'REAL formal matrixはclean worktreeだけで開始できます'}
    if($DryRunDirtyBaseline){throw 'dry-run synthetic dirty baselineをworkload開始前に拒否しました'}
    foreach($pair in @(@($SourceA,$fixtureA),@($SourceB,$fixtureB))){if(-not(Test-Path -LiteralPath $pair[0])){throw "fixtureがありません: $($pair[0])"};if((Hash $pair[0])-ne$pair[1]){throw "fixture hashが違います: $($pair[0])"}}
    if(-not$DryRun -and -not(Test-Path -LiteralPath $Executable)){throw "formal executableがありません: $Executable"}
    $exeHash=if(Test-Path -LiteralPath $Executable){Hash $Executable}else{'DRY_RUN'}
    New-Item -ItemType Directory -Force -Path $OutputDir|Out-Null
    $runs=@();$baselineProvenance=$null
    for($i=1;$i-le3;$i++){
        $raw=Join-Path $OutputDir "run$i-raw.json"
        if($DryRun){$case=if($i-eq$DryRunFailRun){'WrongState'}else{'GoodFormal'};$processExit=Child $generator @('-Output',$raw,'-Case',$case)}
        else{$processExit=(Start-Process -FilePath $Executable -ArgumentList @('--source-a',$SourceA,'--source-b',$SourceB,'--metrics',$raw,'--workload','formal') -Wait -PassThru -NoNewWindow).ExitCode}
        $checkerExit=if(Test-Path -LiteralPath $raw){Child $checker @('-Json',$raw)}else{3}
        $data=if(Test-Path -LiteralPath $raw){Get-Content -Raw -LiteralPath $raw|ConvertFrom-Json}else{$null}
        $provenance=if($data){"$($data.adapter)|$($data.audio_endpoint_sample_rate)|$($data.audio_endpoint_channels)|$($data.audio_endpoint_sample_format)"}else{$null}
        if($i-eq1){$baselineProvenance=$provenance}
        if($DryRunProvenanceChange -and $i-eq3){$provenance='changed-provenance'}
        $runs+=[ordered]@{run=$i;raw_path=(Resolve-Path -LiteralPath $raw).Path;process_exit_code=[int]$processExit;checker_exit_code=[int]$checkerExit;effective_video_fps=$(if($data){$data.effective_video_fps}else{$null});drop_rate=$(if($data){$data.drop_rate}else{$null});av_abs_p95_ms=$(if($data){$data.application_av_delta_abs_ms.p95}else{$null});av_abs_max_ms=$(if($data){$data.application_av_delta_abs_ms.max}else{$null});provenance=$provenance;pass=($processExit-eq0-and$checkerExit-eq0-and$provenance-eq$baselineProvenance)}
    }
    $endHead=(Invoke-Git @('rev-parse','HEAD')).Trim();$endDirty=@(Invoke-Git @('status','--porcelain'));$endClean=$endDirty.Count-eq0
    $endFixtureA=Hash $SourceA;$endFixtureB=Hash $SourceB
    $runProvenanceUnchanged=@($runs|Where-Object{$_.provenance-ne$baselineProvenance}).Count-eq0
    $unchanged=$head-eq$endHead-and$exeHash-eq$(if(Test-Path -LiteralPath $Executable){Hash $Executable}else{'DRY_RUN'})-and$runProvenanceUnchanged-and$endFixtureA-eq$fixtureA-and$endFixtureB-eq$fixtureB
    $allRuns=@($runs|Where-Object{-not$_.pass}).Count-eq0
    if(-not$DryRun){$allRuns=$allRuns-and$startClean-and$endClean-and$unchanged}
    $summary=[ordered]@{schema='mvm-p4-formal-matrix-1';schema_version=1;dry_run=[bool]$DryRun;formal_verdict=$(if($DryRun){'NOT_RUN'}elseif($allRuns){'PASS'}else{'FAIL'});dry_run_harness_pass=$(if($DryRun){$allRuns}else{$false});all_runs_pass=$allRuns;start_head=$head;end_head=$endHead;branch=$branch;start_worktree_clean=$startClean;end_worktree_clean=$endClean;provenance_unchanged=$unchanged;executable_sha256=$exeHash;fixture_a_sha256=$fixtureA;fixture_b_sha256=$fixtureB;schedule_hash=$scheduleHash;adapter=$(if($runs.Count){($runs[0].provenance-split'\|')[0]}else{$null});audio_endpoint=$(if($runs.Count){$runs[0].provenance}else{$null});runs=$runs}
    $summaryPath=Join-Path $OutputDir 'summary.json';$summary|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $summaryPath -Encoding utf8
    if($DryRun){Write-Host "[p4-matrix] dry-run harness pass=$allRuns formal_verdict=NOT_RUN"}else{Write-Host "[p4-matrix] formal all_runs_pass=$allRuns"}
    if(-not$allRuns){exit 3}
}catch{Write-Host "[p4-matrix] FAIL $($_.Exception.Message)";exit 3}finally{Pop-Location}
