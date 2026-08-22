[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CorpusDirectory,
    [Parameter(Mandatory=$true)][string]$RunDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$CorpusDirectory=(Resolve-Path -LiteralPath $CorpusDirectory).Path
$RunDirectory=(Resolve-Path -LiteralPath $RunDirectory).Path
$corpusPath=Join-Path $CorpusDirectory 'corpus-index.json'
$corpusCheckPath=Join-Path $CorpusDirectory 'check-result.json'
$runPath=Join-Path $RunDirectory 'run-index.json'
$corpus=Get-Content -LiteralPath $corpusPath -Raw -Encoding utf8|ConvertFrom-Json
$run=Get-Content -LiteralPath $runPath -Raw -Encoding utf8|ConvertFrom-Json
if($corpus.schema-ne'mvm-p2-r3-synthetic-collision-corpus-1'){Fail 'R3 corpus schemaが不正です'}
if($run.schema-ne'mvm-p2-r4-offline-run-1'){Fail 'R4 run schemaが不正です'}
if((Hash $corpusPath)-ne$run.corpus_index_sha256){Fail 'R3 corpus index hashが不一致です'}
if((Hash $corpusCheckPath)-ne$run.corpus_check_sha256){Fail 'R3 corpus check hashが不一致です'}
if(@($corpus.cases).Count-ne@($run.cases).Count){Fail 'R3/R4 case countが不一致です'}
if([int]$run.case_count-ne52-or@($run.cases).Count-ne52){Fail 'R4 case countは52件でなければなりません'}
$forbidden=@('original_callback_qpc','synthetic_delay_ticks','synthetic_callback_bracket',
    'present_ids','displayed','actual_physical_opportunity_ordinal','expected_solution_class')
$actualCounts=@{};$expectedCounts=@{};$uniqueRecords=0
for($index=0;$index-lt@($corpus.cases).Count;++$index){
    $sourceEntry=$corpus.cases[$index];$runEntry=$run.cases[$index]
    if($sourceEntry.case_id-ne$runEntry.case_id){Fail 'case orderが不一致です'}
    $visiblePath=Join-Path $RunDirectory ([string]$runEntry.visible_input).Replace('/','\')
    $resultPath=Join-Path $RunDirectory ([string]$runEntry.mapper_result).Replace('/','\')
    $hiddenPath=Join-Path $CorpusDirectory ([string]$sourceEntry.hidden_oracle).Replace('/','\')
    if((Hash $visiblePath)-ne$runEntry.visible_input_sha256-or(Hash $resultPath)-ne$runEntry.mapper_result_sha256){
        Fail "R4 case hash不一致: $($runEntry.case_id)"}
    $visibleText=Get-Content -LiteralPath $visiblePath -Raw -Encoding utf8
    foreach($name in $forbidden){if($visibleText-match('"'+[regex]::Escape($name)+'"\s*:')){
        Fail "mapper-visible inputへ禁止fieldが漏れています: $name/$($runEntry.case_id)"}}
    $result=Get-Content -LiteralPath $resultPath -Raw -Encoding utf8|ConvertFrom-Json
    if($result.schema-ne'mvm-p2-r4-mapper-result-1'-or$result.case_id-ne$runEntry.case_id){
        Fail "mapper result identityが不正です: $($runEntry.case_id)"}
    if($result.hidden_oracle_accessed-ne$false){Fail 'mapperがhidden oracle accessを申告しています'}
    if($result.admissibility_relation-ne$run.admissibility_relation){Fail 'admissibility relationが変化しています'}

    # mapper実行が全case完了した後にのみhidden oracleを開く。
    if((Hash $hiddenPath)-ne$sourceEntry.hidden_oracle_sha256){
        Fail "hidden oracle hash不一致: $($runEntry.case_id)"}
    $hidden=Get-Content -LiteralPath $hiddenPath -Raw -Encoding utf8|ConvertFrom-Json
    $expected=[string]$hidden.expected_solution_class;$actual=[string]$result.solution_class
    if(-not$expectedCounts.ContainsKey($expected)){$expectedCounts[$expected]=0};$expectedCounts[$expected]++
    if(-not$actualCounts.ContainsKey($actual)){$actualCounts[$actual]=0};$actualCounts[$actual]++
    if($actual-ne$expected){Fail "solution class不一致: $($runEntry.case_id) expected=$expected actual=$actual"}
    $assignment=@($result.assignment)
    if($actual-eq'UNIQUE'){
        if([int]$result.saturated_solution_count-ne1){Fail 'UNIQUEのsolution countが1ではありません'}
        $oracle=@($hidden.records|ForEach-Object{[int64]$_.actual_physical_opportunity_ordinal})
        if($assignment.Count-ne$oracle.Count){Fail 'UNIQUE assignment countが不一致です'}
        for($record=0;$record-lt$oracle.Count;++$record){
            if([int64]$assignment[$record]-ne$oracle[$record]){
                Fail "UNIQUE identityがETW oracleと不一致です: $($runEntry.case_id)/$record"}}
        $uniqueRecords+=$oracle.Count
    }elseif($assignment.Count-ne0){Fail '非UNIQUEで代表assignmentを返しています'
    }elseif($actual-eq'AMBIGUOUS'-and[int]$result.saturated_solution_count-ne2){
        Fail 'AMBIGUOUSのsolution countが2へsaturateしていません'
    }elseif($actual-eq'NO_SOLUTION'-and[int]$result.saturated_solution_count-ne0){
        Fail 'NO_SOLUTIONのsolution countが0ではありません'}
}
foreach($class in @('UNIQUE','AMBIGUOUS','NO_SOLUTION')){
    if($expectedCounts[$class]-ne$actualCounts[$class]){Fail "$class countが不一致です"}}
if($actualCounts.UNIQUE-ne36-or$actualCounts.AMBIGUOUS-ne8-or$actualCounts.NO_SOLUTION-ne8){
    Fail 'R4 exit criteria 36/8/8と一致しません'}
$resultObject=[ordered]@{
    schema='mvm-p2-r4-proof-1';r4_status='PASS';mapper_proof_status='PASS'
    admissibility_relation=$run.admissibility_relation;case_count=[int]$run.case_count
    solution_class_counts=$actualCounts;unique_identity_exact_record_count=$uniqueRecords
    hidden_oracle_was_absent_from_mapper_input=$true
    unpresented_scope='NOT_PROVEN_SOURCE_CORPUS_900_OF_900_DISPLAYED'
    next='F3-B1_RUNTIME_WIRING'
}
$resultObject|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $RunDirectory 'proof-result.json') -Encoding utf8
Write-Host "R4 offline mapper proof: PASS (UNIQUE=36 AMBIGUOUS=8 NO_SOLUTION=8)"
