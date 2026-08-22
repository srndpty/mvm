[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CorpusDirectory,
    [string]$SourceArtifactDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'bench\results\f3-b0.6-r2-20260822-1919'),
    [string]$OracleChecker = (Join-Path $PSScriptRoot 'check-p2-etw-present-history.ps1'),
    [string]$VBlankChecker = (Join-Path $PSScriptRoot 'check-p2-vblank-shadow.ps1')
)

$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message) { throw $Message }
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Canonical($Value) { $Value | ConvertTo-Json -Depth 30 -Compress }
function Count-Solutions([int]$Opportunities, [int]$Records) {
    $count=0
    function Visit([int]$Next, [int]$Remaining) {
        if ($Remaining -eq 0) { $script:solutionCounter++; return }
        for ($candidate=$Next; $candidate -le $Opportunities-$Remaining; ++$candidate) {
            Visit ($candidate+1) ($Remaining-1)
        }
    }
    $script:solutionCounter=0; Visit 0 $Records; $count=$script:solutionCounter
    return $count
}
function Bracket($Samples, [int64]$Qpc) {
    for ($i=0;$i+1-lt$Samples.Count;++$i) {
        if ([int64]$Samples[$i].qpc-le$Qpc-and$Qpc-lt[int64]$Samples[$i+1].qpc) {
            return [int64]$Samples[$i].ordinal
        }
    }
    return $null
}

$CorpusDirectory=(Resolve-Path -LiteralPath $CorpusDirectory).Path
$SourceArtifactDirectory=(Resolve-Path -LiteralPath $SourceArtifactDirectory).Path
$indexPath=Join-Path $CorpusDirectory 'corpus-index.json'
$sourceAppPath=Join-Path $SourceArtifactDirectory 'traced-app.json'
$sourceEtwPath=Join-Path $SourceArtifactDirectory 'present-history-raw.json'
foreach($path in @($indexPath,$sourceAppPath,$sourceEtwPath,$OracleChecker,$VBlankChecker)) {
    if(-not(Test-Path -LiteralPath $path)){Fail "必須fileがありません: $path"}
}
$index=Get-Content -LiteralPath $indexPath -Raw -Encoding utf8|ConvertFrom-Json
if($index.schema-ne'mvm-p2-r3-synthetic-collision-corpus-1'){Fail 'corpus schemaが不正です'}
if((Hash $sourceAppPath)-ne$index.source_hashes.app){Fail 'source app hashが不一致です'}
if((Hash $sourceEtwPath)-ne$index.source_hashes.etw){Fail 'source ETW hashが不一致です'}
$sourceApp=Get-Content -LiteralPath $sourceAppPath -Raw -Encoding utf8|ConvertFrom-Json
$sourceSamples=@($sourceApp.presentation_opportunity.physical_vblank.samples)
$sourceSwaps=@($sourceApp.presentation_opportunity.swap_records)
$tempOracle=Join-Path $CorpusDirectory '.r3-check-oracle.tmp.json'
try {
    & pwsh -NoProfile -File $OracleChecker -AppJson $sourceAppPath -EtwJson $sourceEtwPath `
        -Output $tempOracle -VBlankChecker $VBlankChecker *> $null
    if($LASTEXITCODE-ne0){Fail 'source oracleの独立再計算に失敗しました'}
    $sourceOracle=Get-Content -LiteralPath $tempOracle -Raw -Encoding utf8|ConvertFrom-Json
} finally {
    if(Test-Path -LiteralPath $tempOracle){Remove-Item -LiteralPath $tempOracle -Force}
}
$sourceRecords=@($sourceOracle.records)
if($sourceOracle.oracle_status-ne'ORACLE_VALID'){Fail 'source oracleがVALIDではありません'}
if($sourceOracle.collision_evidence_status-ne'COLLISION_NOT_OBSERVED'){
    Fail 'source collision状態が固定値と違います'
}
if(@($index.cases).Count-ne[int]$index.case_count){Fail 'case_countが不一致です'}
$forbidden=@('present_ids','displayed','actual_physical_opportunity_ordinal','expected_solution_class')
$observedClasses=@{}
foreach($entry in @($index.cases)) {
    $mapperPath=Join-Path $CorpusDirectory ([string]$entry.mapper_input).Replace('/','\')
    $hiddenPath=Join-Path $CorpusDirectory ([string]$entry.hidden_oracle).Replace('/','\')
    if((Hash $mapperPath)-ne$entry.mapper_input_sha256){Fail "mapper hash不一致: $($entry.case_id)"}
    if((Hash $hiddenPath)-ne$entry.hidden_oracle_sha256){Fail "hidden hash不一致: $($entry.case_id)"}
    $mapper=Get-Content -LiteralPath $mapperPath -Raw -Encoding utf8|ConvertFrom-Json
    $hidden=Get-Content -LiteralPath $hiddenPath -Raw -Encoding utf8|ConvertFrom-Json
    if($mapper.case_id-ne$entry.case_id-or$hidden.case_id-ne$entry.case_id){Fail 'case id不一致'}
    $mapperText=Get-Content -LiteralPath $mapperPath -Raw -Encoding utf8
    foreach($name in $forbidden){if($mapperText-match('"'+[regex]::Escape($name)+'"\s*:')){
        Fail "mapper inputへoracle fieldが漏れています: $name/$($entry.case_id)"}}
    if([int]$mapper.sync_interval-ne1){Fail "SyncIntervalが1ではありません: $($entry.case_id)"}
    $samples=@($mapper.vblank_samples);$records=@($mapper.records);$hiddenRecords=@($hidden.records)
    if($samples.Count-lt2){Fail 'VBlank sliceが不足しています'}
    for($i=0;$i-lt$samples.Count;++$i){
        $sourceIndex=[int]$samples[$i].source_sample_index
        if($sourceIndex-lt0-or$sourceIndex-ge$sourceSamples.Count){Fail 'source sample indexが範囲外です'}
        if([int64]$samples[$i].ordinal-ne[int64]$sourceSamples[$sourceIndex].ordinal-or
           [int64]$samples[$i].qpc-ne[int64]$sourceSamples[$sourceIndex].qpc){
            Fail "VBlank sampleがsourceから変化しています: $($entry.case_id)/$i"
        }
        if($i-gt0-and[int]$samples[$i].source_sample_index-ne[int]$samples[$i-1].source_sample_index+1){
            Fail "VBlank sliceが連続していません: $($entry.case_id)"
        }
    }
    if($records.Count-ne$hiddenRecords.Count){Fail 'visible/hidden record count不一致'}
    $previousSynthetic=$null;$brackets=@()
    for($i=0;$i-lt$records.Count;++$i){
        $submission=[int]$records[$i].submission_index
        if($submission-ne[int]$hiddenRecords[$i].submission_index){Fail 'record orderが不一致です'}
        if($submission-lt0-or$submission-ge$sourceRecords.Count){Fail 'submission indexが範囲外です'}
        $original=[int64]$records[$i].original_callback_qpc
        $synthetic=[int64]$records[$i].synthetic_callback_qpc
        $delay=[int64]$records[$i].synthetic_delay_ticks
        if($original-ne[int64]$sourceSwaps[$submission].swap_qpc){Fail 'original callbackがsourceと不一致です'}
        if($delay-lt0-or$synthetic-$original-ne$delay){Fail 'synthetic delay式が不正です'}
        if($null-ne$previousSynthetic-and$synthetic-le$previousSynthetic){Fail 'callback orderが不正です'}
        $previousSynthetic=$synthetic
        $bracket=Bracket $samples $synthetic
        if($null-eq$bracket-or$bracket-ne[int64]$records[$i].synthetic_callback_bracket){
            Fail "synthetic bracketが再計算と不一致です: $($entry.case_id)/$i"
        }
        $brackets+=$bracket
        $source=$sourceRecords[$submission];$candidate=$hiddenRecords[$i]
        if($candidate.status-ne$source.status-or
           [int64]$candidate.actual_physical_opportunity_ordinal-ne[int64]$source.first_opportunity_ordinal-or
           (Canonical @($candidate.present_ids))-ne(Canonical @($source.present_ids))-or
           (Canonical @($candidate.displayed))-ne(Canonical @($source.displayed))){
            Fail "hidden oracle identityがsourceから変化しています: $($entry.case_id)/$i"
        }
    }
    if(@($brackets|Select-Object -Unique).Count-ne1){Fail "collisionが成立しません: $($entry.case_id)"}
    $actual=@($hiddenRecords|ForEach-Object{[int64]$_.actual_physical_opportunity_ordinal})
    if(@($actual|Select-Object -Unique).Count-ne$actual.Count){Fail 'actual opportunityがinjectiveではありません'}
    $opportunities=$samples.Count-1
    if($opportunities-ne[int]$mapper.mapper_visible_opportunity_count){Fail 'visible opportunity count不一致'}
    $solutions=Count-Solutions $opportunities $records.Count
    $class=if($solutions-eq0){'NO_SOLUTION'}elseif($solutions-eq1){'UNIQUE'}else{'AMBIGUOUS'}
    if($solutions-ne[int64]$hidden.expected_solution_count-or$class-ne$hidden.expected_solution_class-or
       $class-ne$entry.expected_solution_class){Fail "solution class不一致: $($entry.case_id)"}
    if(-not$observedClasses.ContainsKey($class)){$observedClasses[$class]=0};$observedClasses[$class]++
}
foreach($required in @('UNIQUE','NO_SOLUTION','AMBIGUOUS')){
    if(-not$observedClasses.ContainsKey($required)-or$observedClasses[$required]-lt1){Fail "$required fixtureがありません"}
}
[ordered]@{
    schema='mvm-p2-r3-corpus-check-1'; corpus_status='PASS'; mapper_proof_status='NOT_YET_EVALUABLE'
    mapper_changed=$false; case_count=[int]$index.case_count; solution_class_counts=$observedClasses
    source_oracle_status=$sourceOracle.oracle_status
    source_collision_status=$sourceOracle.collision_evidence_status
}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $CorpusDirectory 'check-result.json') -Encoding utf8
Write-Host "R3 synthetic collision corpus: PASS ($($index.case_count) cases)"
