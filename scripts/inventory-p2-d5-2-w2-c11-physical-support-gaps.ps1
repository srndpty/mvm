[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$C011Directory,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$InventoryProof=(Join-Path $C011Directory 'display-candidate-inventory.json')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Need($Object,[string]$Name){
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){Fail "必須fieldがありません: $Name"}
    return $Object.$Name
}
function Count-ByField([array]$Values,[string]$Field){
    $counts=[ordered]@{}
    foreach($group in @($Values|Group-Object -Property $Field|Sort-Object Name)){$counts[[string]$group.Name]=$group.Count}
    return $counts
}
if(-not(Test-Path -LiteralPath $C011Directory)){Fail "C0.1.1 directoryがありません: $C011Directory"}
if(-not(Test-Path -LiteralPath $InventoryProof)){Fail "C0.1.1 inventory proofがありません: $InventoryProof"}
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c1-mapping-core.ps1')
$inventory=Get-Content -LiteralPath $InventoryProof -Raw -Encoding utf8|ConvertFrom-Json
$runCount=[int](Need $inventory 'run_count')
if($runCount-le0-or@($inventory.runs).Count-ne$runCount){Fail 'inventory run数が不正です'}
$runProofs=@();$totalCandidates=0L;$totalMapped=0L;$totalMissing=0L
$totalLower=0L;$totalUpper=0L;$totalInside=0L;$totalAmbiguous=0L;$allDiagnosed=$true
for($run=1;$run-le$runCount;++$run){
    $appPath=Join-Path (Join-Path $C011Directory "run-$run") 'traced-app.json'
    if(-not(Test-Path -LiteralPath $appPath)){Fail "run $run traced-app.jsonがありません"}
    $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
    $opportunity=Need $app 'presentation_opportunity'
    $physical=Need $opportunity 'physical_vblank'
    $shadow=Need $opportunity 'physical_vblank_domain_shadow'
    $samples=@(Need $physical 'samples')
    if($samples.Count-lt2){Fail "run $run physical sampleが不足しています"}
    $predecessorOrdinal=[int64](Need $shadow 'predecessor_ordinal')
    $successorOrdinal=[int64](Need $shadow 'successor_ordinal')
    $predecessorSamples=@($samples|Where-Object{[int64]$_.ordinal-eq$predecessorOrdinal})
    $successorSamples=@($samples|Where-Object{[int64]$_.ordinal-eq$successorOrdinal})
    if($predecessorSamples.Count-ne1-or$successorSamples.Count-ne1){Fail "run $run boundary sample witnessがexactではありません"}
    $predecessorQpc=[int64]$predecessorSamples[0].qpc
    $successorQpc=[int64]$successorSamples[0].qpc
    if($predecessorQpc-ge$successorQpc){Fail "run $run physical support境界が不正です"}
    $candidates=@($inventory.runs[$run-1].candidates)
    $mapping=Invoke-MvmDisplayedQpcPhysicalMapping -Candidates $candidates -Samples $samples `
        -PredecessorOrdinal $predecessorOrdinal -SuccessorOrdinal $successorOrdinal `
        -OriginOrdinal ([int64](Need $shadow 'origin_ordinal')) -LastOrdinal ([int64](Need $shadow 'last_ordinal')) `
        -PhysicalAuthorityValid $true
    $missingDetails=@();$lower=0;$upper=0;$inside=0;$relationExact=$true;$withinCount=0;$withinMapped=0
    $relationScope=[ordered]@{}
    for($index=0;$index-lt$candidates.Count;++$index){
        $candidate=$candidates[$index];$record=$mapping.records[$index]
        $displayed=@(Need $candidate 'displayed_qpc')
        if($displayed.Count-ne1){Fail "run $run candidate $index DisplayedQPC cardinalityが不正です"}
        $qpc=[int64]$displayed[0]
        $expectedRelation=$(if($qpc-lt$predecessorQpc){'BEFORE_PREDECESSOR'}elseif($qpc-gt$successorQpc){'AFTER_SUCCESSOR'}else{'WITHIN_PREDECESSOR_SUCCESSOR_ENVELOPE'})
        $actualRelation=[string](Need $candidate 'display_relation')
        if($actualRelation-ne$expectedRelation){$relationExact=$false}
        $scope=[string](Need $candidate 'intent_scope')
        if([string]::IsNullOrEmpty($scope)){$scope='__MISSING__'}
        if(-not$relationScope.Contains($actualRelation)){$relationScope[$actualRelation]=[ordered]@{}}
        if(-not$relationScope[$actualRelation].Contains($scope)){$relationScope[$actualRelation][$scope]=0}
        $relationScope[$actualRelation][$scope]=[int]$relationScope[$actualRelation][$scope]+1
        if($actualRelation-eq'WITHIN_PREDECESSOR_SUCCESSOR_ENVELOPE'){
            ++$withinCount
            if([bool]$record.mapping_exact){++$withinMapped}
        }
        if(-not[bool]$record.mapping_exact-and[int]$record.mapping_solution_count-eq0){
            $gap=$(if($qpc-lt$predecessorQpc){$lower+=1;'LOWER_SUPPORT_INSUFFICIENT'}elseif($qpc-gt$successorQpc){$upper+=1;'UPPER_SUPPORT_INSUFFICIENT'}else{$inside+=1;'INSIDE_SUPPORT_CELL_MISSING'})
            $missingDetails+=[ordered]@{
                etw_sequence=$candidate.etw_sequence;displayed_qpc=$qpc
                display_relation=$actualRelation;intent_scope=$scope;support_gap=$gap
            }
        }
    }
    $missing=[int]$mapping.missing_mapping_count;$ambiguous=[int]$mapping.ambiguous_mapping_count
    $diagnosed=$missing-eq($lower+$upper)-and$inside-eq0-and$ambiguous-eq0-and
        $withinMapped-eq$withinCount-and$relationExact
    if(-not$diagnosed){$allDiagnosed=$false}
    $displayQpcs=@($candidates|ForEach-Object{[int64](Need $_ 'displayed_qpc')})
    $runProofs+=[ordered]@{
        run=$run;presented_candidate_count=$candidates.Count
        display_relation_counts=(Count-ByField $candidates 'display_relation')
        display_relation_by_intent_scope=$relationScope
        earliest_displayed_qpc=($displayQpcs|Measure-Object -Minimum).Minimum
        latest_displayed_qpc=($displayQpcs|Measure-Object -Maximum).Maximum
        physical_sample_count=$samples.Count
        physical_first_sample_qpc=[int64]$samples[0].qpc
        physical_last_sample_qpc=[int64]$samples[-1].qpc
        predecessor_ordinal=$predecessorOrdinal;predecessor_qpc=$predecessorQpc
        successor_ordinal=$successorOrdinal;successor_qpc=$successorQpc
        mapped_exact_count=[int]$mapping.mapped_exact_count
        missing_mapping_count=$missing;ambiguous_mapping_count=$ambiguous
        lower_support_insufficient_count=$lower;upper_support_insufficient_count=$upper
        inside_support_unmapped_count=$inside
        within_support_candidate_count=$withinCount;within_support_mapped_exact_count=$withinMapped
        relation_classification_exact=$relationExact
        missing_explained_by_closed_support_only=$diagnosed
        missing_candidates=$missingDetails
    }
    $totalCandidates+=$candidates.Count;$totalMapped+=[int64]$mapping.mapped_exact_count
    $totalMissing+=$missing;$totalLower+=$lower;$totalUpper+=$upper;$totalInside+=$inside;$totalAmbiguous+=$ambiguous
}
$incomplete=$allDiagnosed-and$totalMissing-gt0
$result=[ordered]@{
    schema='mvm-p2-d5-2-w2-c11-physical-support-gap-inventory-1'
    stage='P2-D5-2-W2-C1.1';source_c011_directory=(Resolve-Path -LiteralPath $C011Directory).Path
    source_inventory_sha256=((Get-FileHash -LiteralPath $InventoryProof -Algorithm SHA256).Hash.ToLowerInvariant())
    run_count=$runCount;presented_candidate_count=$totalCandidates;mapped_exact_count=$totalMapped
    missing_mapping_count=$totalMissing;ambiguous_mapping_count=$totalAmbiguous
    before_physical_support_count=$totalLower;after_physical_support_count=$totalUpper
    inside_support_unmapped_count=$totalInside
    missing_equals_outside_support_count=$totalMissing-eq($totalLower+$totalUpper)
    support_gap_diagnosis_exact=$allDiagnosed
    sealed_acquisition_mapping_valid=$false
    verdict=$(if($incomplete){'PHYSICAL_MAPPING_SUPPORT_ENVELOPE_INCOMPLETE'}else{'PHYSICAL_MAPPING_SUPPORT_GAP_DIAGNOSIS_INVALID'})
    runs=$runProofs
}
$outputDirectory=Split-Path -Parent $Output
if($outputDirectory-and-not(Test-Path -LiteralPath $outputDirectory)){New-Item -ItemType Directory -Path $outputDirectory|Out-Null}
$result|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
if(-not$incomplete){Fail 'physical mapping support gapをexactに説明できません'}
Write-Host "P2-D5-2 W2-C1.1 support gap: $($result.verdict) ($totalMissing = $totalLower + $totalUpper)"
