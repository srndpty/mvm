[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CorpusDirectory,
    [Parameter(Mandatory=$true)][string]$R4Directory,
    [Parameter(Mandatory=$true)][string]$RunDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Get-Solutions([object[]]$Callbacks,[object[]]$Opportunities){
    $solutions=[Collections.ArrayList]::new();$assignment=[Collections.ArrayList]::new()
    $visit=$null
    $visit={param([int]$Record,[int]$Next)
        if($Record-eq$Callbacks.Count){[void]$solutions.Add([pscustomobject]@{assignment=@($assignment)});return}
        for($opportunity=$Next;$opportunity-lt$Opportunities.Count;++$opportunity){
            if([int64]$Opportunities[$opportunity].qpc-le[int64]$Callbacks[$Record]){
                [void]$assignment.Add([int64]$Opportunities[$opportunity].ordinal)
                &$visit ($Record+1) ($opportunity+1)
                $assignment.RemoveAt($assignment.Count-1)
            }
        }
    }
    &$visit 0 0
    return @($solutions)
}
function Get-Consensus([object[]]$Solutions){
    $result=@();if($Solutions.Count-eq0){return $result}
    for($record=0;$record-lt@($Solutions[0].assignment).Count;++$record){
        $value=[int64]$Solutions[0].assignment[$record]
        if(@($Solutions|Where-Object{[int64]$_.assignment[$record]-ne$value}).Count-ne0){break}
        $result+=$value
    }
    return $result
}
function Equal-Array([object[]]$Left,[object[]]$Right){
    if($Left.Count-ne$Right.Count){return $false}
    for($index=0;$index-lt$Left.Count;++$index){if([int64]$Left[$index]-ne[int64]$Right[$index]){return $false}}
    return $true
}
$CorpusDirectory=(Resolve-Path -LiteralPath $CorpusDirectory).Path
$R4Directory=(Resolve-Path -LiteralPath $R4Directory).Path
$RunDirectory=(Resolve-Path -LiteralPath $RunDirectory).Path
$corpusPath=Join-Path $CorpusDirectory 'corpus-index.json'
$r4RunPath=Join-Path $R4Directory 'run-index.json';$r4ProofPath=Join-Path $R4Directory 'proof-result.json'
$runPath=Join-Path $RunDirectory 'run-index.json'
$corpus=Get-Content -LiteralPath $corpusPath -Raw -Encoding utf8|ConvertFrom-Json
$r4Run=Get-Content -LiteralPath $r4RunPath -Raw -Encoding utf8|ConvertFrom-Json
$r4Proof=Get-Content -LiteralPath $r4ProofPath -Raw -Encoding utf8|ConvertFrom-Json
$run=Get-Content -LiteralPath $runPath -Raw -Encoding utf8|ConvertFrom-Json
if($r4Proof.r4_status-ne'PASS'){Fail 'R4 proofがPASSではありません'}
if($run.schema-ne'mvm-p2-b1a-incremental-run-1'){Fail 'B1a run schemaが不正です'}
if((Hash $r4RunPath)-ne$run.r4_run_sha256-or(Hash $r4ProofPath)-ne$run.r4_proof_sha256){Fail 'R4 provenance hashが不一致です'}
if(@($run.cases).Count-ne52-or@($r4Run.cases).Count-ne52-or@($corpus.cases).Count-ne52){Fail 'B1a case countは52件でなければなりません'}
$counts=@{};$uniqueRecords=0;$commitEvents=0
for($caseIndex=0;$caseIndex-lt52;++$caseIndex){
    $entry=$run.cases[$caseIndex];$r4Entry=$r4Run.cases[$caseIndex];$corpusEntry=$corpus.cases[$caseIndex]
    if($entry.case_id-ne$r4Entry.case_id-or$entry.case_id-ne$corpusEntry.case_id){Fail 'case orderが不一致です'}
    $visiblePath=Join-Path $R4Directory ([string]$entry.r4_visible_input).Replace('/','\')
    $offlinePath=Join-Path $R4Directory ([string]$entry.r4_offline_result).Replace('/','\')
    $incrementalPath=Join-Path $RunDirectory ([string]$entry.incremental_result).Replace('/','\')
    if((Hash $visiblePath)-ne$r4Entry.visible_input_sha256-or(Hash $offlinePath)-ne$r4Entry.mapper_result_sha256-or
       (Hash $incrementalPath)-ne$entry.incremental_result_sha256){Fail "case hash不一致: $($entry.case_id)"}
    $visible=Get-Content -LiteralPath $visiblePath -Raw -Encoding utf8|ConvertFrom-Json
    $offline=Get-Content -LiteralPath $offlinePath -Raw -Encoding utf8|ConvertFrom-Json
    $incremental=Get-Content -LiteralPath $incrementalPath -Raw -Encoding utf8|ConvertFrom-Json
    if($incremental.schema-ne'mvm-p2-b1a-incremental-result-1'-or$incremental.hidden_oracle_accessed-ne$false){Fail 'incremental result contractが不正です'}
    if($incremental.solution_class-ne$offline.solution_class){Fail "offline/incremental class不一致: $($entry.case_id)"}
    if(-not$counts.ContainsKey([string]$incremental.solution_class)){$counts[[string]$incremental.solution_class]=0};$counts[[string]$incremental.solution_class]++
    $arrivedVblank=@();$arrivedCallback=@();$previousCommit=@();$lastQpc=[int64]::MinValue
    foreach($mappingEvent in @($incremental.events)){
        $qpc=[int64]$mappingEvent.qpc
        if($qpc-lt$lastQpc){Fail "event QPC regression: $($entry.case_id)"};$lastQpc=$qpc
        if($mappingEvent.event_type-eq'VBLANK'){$arrivedVblank+=@($visible.vblank_samples|Where-Object{[int64]$_.qpc-eq$qpc}|Select-Object -First 1)
        }elseif($mappingEvent.event_type-eq'CALLBACK'){$arrivedCallback+=$qpc}
        $actualCommit=@($mappingEvent.committed_assignment)
        if($actualCommit.Count-lt$previousCommit.Count){Fail "commit watermark regression: $($entry.case_id)"}
        for($committed=0;$committed-lt$previousCommit.Count;++$committed){
            if([int64]$actualCommit[$committed]-ne[int64]$previousCommit[$committed]){
                Fail "committed identity regression: $($entry.case_id)"}}
        if($arrivedVblank.Count-ge2-and$arrivedCallback.Count-ne0){
            $boundary=[int64]$arrivedVblank[-1].qpc
            $closed=@($arrivedCallback|Where-Object{[int64]$_-lt$boundary})
            if($closed.Count-ne0){
                $opportunities=@($arrivedVblank[0..($arrivedVblank.Count-2)])
                $solutions=@(Get-Solutions $closed $opportunities)
                $consensus=@(Get-Consensus $solutions)
                if($solutions.Count-ne0-and-not(Equal-Array $actualCommit $consensus)){Fail "consensus prefix不一致: $($entry.case_id)/$qpc"}
            }
        }
        if(@($mappingEvent.newly_committed).Count-ne0){$commitEvents++}
        $previousCommit=$actualCommit
    }
    if($incremental.solution_class-eq'UNIQUE'){
        if(-not(Equal-Array @($incremental.final_assignment) @($offline.assignment))){Fail "UNIQUE final assignment不一致: $($entry.case_id)"}
        $hiddenPath=Join-Path $CorpusDirectory ([string]$corpusEntry.hidden_oracle).Replace('/','\')
        if((Hash $hiddenPath)-ne$corpusEntry.hidden_oracle_sha256){Fail "hidden oracle hash不一致: $($entry.case_id)"}
        $hidden=Get-Content -LiteralPath $hiddenPath -Raw -Encoding utf8|ConvertFrom-Json
        $oracle=@($hidden.records|ForEach-Object{[int64]$_.actual_physical_opportunity_ordinal})
        if(-not(Equal-Array @($incremental.final_assignment) $oracle)){Fail "UNIQUE ETW identity不一致: $($entry.case_id)"}
        $uniqueRecords+=$oracle.Count
    }
}
if($counts.UNIQUE-ne36-or$counts.AMBIGUOUS-ne8-or$counts.NO_SOLUTION-ne8){Fail 'B1a exit criteria 36/8/8と一致しません'}
$proof=[ordered]@{
    schema='mvm-p2-b1a-incremental-proof-1';b1a_status='PASS';case_count=52
    solution_class_counts=$counts;unique_identity_exact_record_count=$uniqueRecords
    consensus_commit_event_count=$commitEvents;commit_regression_count=0
    first_solution_early_commit_allowed=$false;next='F3-B1B_LIVE_SHADOW_WIRING'
}
$proof|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $RunDirectory 'proof-result.json') -Encoding utf8
Write-Host "B1a incremental proof: PASS (36/8/8, UNIQUE records=$uniqueRecords)"
