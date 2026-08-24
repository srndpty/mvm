[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$C011Directory,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$Inventory=(Join-Path $PSScriptRoot 'inventory-p2-d5-2-w2-c0-display-candidates.ps1'),
    [string]$PhysicalChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2a-physical-domain.ps1'),
    [string]$EnvelopeChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c01-capture-envelope.ps1'),
    [string]$SupportChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c11-physical-support-envelope.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Need($Object,[string]$Name){
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){Fail "必須fieldがありません: $Name"}
    return $Object.$Name
}
foreach($path in @($C011Directory,$Inventory,$PhysicalChecker,$EnvelopeChecker,$SupportChecker)){
    if(-not(Test-Path -LiteralPath $path)){Fail "W2-C1必須pathがありません: $path"}
}
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c1-mapping-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c13-formal-population-core.ps1')
$outputDirectory=Split-Path -Parent $Output
if([string]::IsNullOrWhiteSpace($outputDirectory)){$outputDirectory='.'}
if(-not(Test-Path -LiteralPath $outputDirectory)){New-Item -ItemType Directory -Path $outputDirectory|Out-Null}
$upstreamProof=Join-Path $outputDirectory (([IO.Path]::GetFileNameWithoutExtension($Output))+'.upstream-c011.json')
& pwsh -NoProfile -File $Inventory -B2LiveDirectory $C011Directory -Output $upstreamProof -RequireCoverageComplete *> $null
if($LASTEXITCODE-ne0){Fail 'W2-C0.1.1 upstream coverage authorityが不成立です'}
$inventoryProof=Get-Content -LiteralPath $upstreamProof -Raw -Encoding utf8|ConvertFrom-Json
if(-not[bool](Need $inventoryProof 'coverage_complete')-or-not[bool](Need $inventoryProof 'intent_scope_exact')-or
   [int64](Need $inventoryProof 'missing_scope_count')-ne0-or
   [int64](Need $inventoryProof 'ambiguous_scope_count')-ne0-or
   [int64](Need $inventoryProof 'mutated_scope_count')-ne0){Fail 'W2-C0.1.1 intent scope authorityが不成立です'}
$runCount=[int](Need $inventoryProof 'run_count');if($runCount-le0){Fail 'W2-C1 run数が不正です'}
$runResults=@();$globalBlockers=@{}
for($run=1;$run-le$runCount;++$run){
    $runDirectory=Join-Path $C011Directory "run-$run"
    $appPath=Join-Path $runDirectory 'traced-app.json'
    $etwPath=Join-Path $runDirectory 'present-history-raw.json'
    $terminalPath=Join-Path $runDirectory 'terminal-shadow.json'
    foreach($path in @($appPath,$etwPath,$terminalPath)){if(-not(Test-Path -LiteralPath $path)){Fail "run $run artifactがありません: $path"}}
    $physicalProof=Join-Path $outputDirectory "w2-c1-run-$run-physical-proof.json"
    & pwsh -NoProfile -File $PhysicalChecker -Json $appPath -Output $physicalProof *> $null
    $physicalExit=$LASTEXITCODE
    $physicalValid=$physicalExit-eq0-and(Test-Path -LiteralPath $physicalProof)
    if($physicalValid){
        $physicalCheck=Get-Content -LiteralPath $physicalProof -Raw -Encoding utf8|ConvertFrom-Json
        $physicalValid=[bool](Need $physicalCheck 'shadow_authority_valid')
    }
    $envelopeProof=Join-Path $outputDirectory "w2-c1-run-$run-envelope-proof.json"
    & pwsh -NoProfile -File $EnvelopeChecker -InputJson $appPath -Output $envelopeProof *> $null
    $envelopeValid=$LASTEXITCODE-eq0
    $supportProof=Join-Path $outputDirectory "w2-c1-run-$run-mapping-support-proof.json"
    & pwsh -NoProfile -File $SupportChecker -InputJson $appPath -Output $supportProof *> $null
    $supportValid=$LASTEXITCODE-eq0
    $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
    $etw=Get-Content -LiteralPath $etwPath -Raw -Encoding utf8|ConvertFrom-Json
    $terminal=Get-Content -LiteralPath $terminalPath -Raw -Encoding utf8|ConvertFrom-Json
    if([string](Need $terminal 'schema')-ne'mvm-p2-d5-2-w2-b2-terminal-shadow-1'-or
       [string](Need $terminal 'verdict')-ne'NATIVE_PRESENT_TERMINAL_OUTCOME_EXACT'){
        Fail "run $run B2 terminal authorityが不正です"
    }
    $opportunity=Need $app 'presentation_opportunity';$physical=Need $opportunity 'physical_vblank'
    $shadow=Need $opportunity 'physical_vblank_domain_shadow'
    $mappingSupport=Need $opportunity 'physical_mapping_support_envelope_shadow'
    $scope=Need (Need $app 'native_present_hook') 'intent_scope_provenance'
    $runInventory=$inventoryProof.runs[$run-1]
    $observedCandidates=@($runInventory.candidates)
    $formalPopulation=Invoke-MvmC13FormalPresentedPopulation -ObservedCandidates $observedCandidates `
        -B2TerminalRecords @(Need $terminal 'records')
    # formal membershipはB2 terminal authorityだけが定義する。C0 candidateは
    # exact event identityによるenrichmentにだけ使用する。
    $presentedCandidates=@($formalPopulation.formal_candidates)
    [void]$formalPopulation.Remove('formal_candidates')
    $upstreamValid=$physicalValid-and$envelopeValid-and$supportValid-and[bool](Need $runInventory 'coverage_complete')-and
        [bool](Need $runInventory 'intent_scope_exact')-and[bool](Need $scope 'authority_pass')
    $mapping=Invoke-MvmDisplayedQpcPhysicalMapping -Candidates $presentedCandidates `
        -Samples @(Need $physical 'samples') `
        -PredecessorOrdinal ([int64](Need $mappingSupport 'predecessor_ordinal')) `
        -SuccessorOrdinal ([int64](Need $mappingSupport 'successor_ordinal')) `
        -OriginOrdinal ([int64](Need $shadow 'origin_ordinal')) `
        -LastOrdinal ([int64](Need $shadow 'last_ordinal')) `
        -PhysicalAuthorityValid $upstreamValid `
        -EtwEventsLost ([int64](Need $etw 'etw_events_lost')) `
        -EtwBuffersLost ([int64](Need $etw 'etw_buffers_lost')) `
        -PresentEventOverflowCount ([int64](Need $etw 'present_event_overflow_count'))
    $observedMapping=Invoke-MvmDisplayedQpcPhysicalMapping -Candidates $observedCandidates `
        -Samples @(Need $physical 'samples') `
        -PredecessorOrdinal ([int64](Need $mappingSupport 'predecessor_ordinal')) `
        -SuccessorOrdinal ([int64](Need $mappingSupport 'successor_ordinal')) `
        -OriginOrdinal ([int64](Need $shadow 'origin_ordinal')) `
        -LastOrdinal ([int64](Need $shadow 'last_ordinal')) `
        -PhysicalAuthorityValid $upstreamValid `
        -EtwEventsLost ([int64](Need $etw 'etw_events_lost')) `
        -EtwBuffersLost ([int64](Need $etw 'etw_buffers_lost')) `
        -PresentEventOverflowCount ([int64](Need $etw 'present_event_overflow_count'))
    foreach($blocker in @($formalPopulation.blockers)){$mapping.blockers+=,[string]$blocker;$mapping.mapping_exact=$false}
    foreach($blocker in @($mapping.blockers)){$globalBlockers[[string]$blocker]=$true}
    $mapping.run=$run
    $mapping.upstream_authority_valid=$upstreamValid
    $mapping.capture_envelope_valid=$envelopeValid
    $mapping.physical_mapping_support_envelope_valid=$supportValid
    $mapping.physical_authority_valid=$physicalValid
    $mapping.intent_scope_exact=[bool]$runInventory.intent_scope_exact
    $mapping.inventory_presented_candidate_count=$observedCandidates.Count
    $mapping.observed_presented_candidate_count=$observedCandidates.Count
    $mapping.formal_presented_candidate_count=$presentedCandidates.Count
    $mapping.candidate_count_identity=$mapping.presented_candidate_count-eq[long]$formalPopulation.b2_formal_presented_count
    $mapping.formal_population_authority=$formalPopulation
    $mapping.observed_physical_mapping_diagnostic=$observedMapping
    $mapping.sealed_input_sha256=[ordered]@{
        traced_app=((Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash.ToLowerInvariant())
        present_history_raw=((Get-FileHash -LiteralPath $etwPath -Algorithm SHA256).Hash.ToLowerInvariant())
        b2_terminal_shadow=((Get-FileHash -LiteralPath $terminalPath -Algorithm SHA256).Hash.ToLowerInvariant())
        upstream_inventory_proof=((Get-FileHash -LiteralPath $upstreamProof -Algorithm SHA256).Hash.ToLowerInvariant())
    }
    if(-not$mapping.candidate_count_identity){
        $mapping.blockers+=,'CANDIDATE_COUNT_IDENTITY_MISMATCH'
        $mapping.mapping_exact=$false
        $globalBlockers['CANDIDATE_COUNT_IDENTITY_MISMATCH']=$true
    }
    $runResults+=,$mapping
}
$globalBlockerList=@($globalBlockers.Keys|Sort-Object)
$presented=0L;$mapped=0L;$inDomain=0L;$outDomain=0L;$missing=0L;$ambiguous=0L;$duplicate=0L
$upstreamInvalid=0L;$observedPresented=0L;$nonformalObserved=0L;$invalidNonformal=0L;$exactNonformal=0L
$observedMapped=0L;$observedMissing=0L;$observedAmbiguous=0L;$observedDuplicate=0L
foreach($runResult in $runResults){
    $presented+=[int64]$runResult.presented_candidate_count;$mapped+=[int64]$runResult.mapped_exact_count
    $inDomain+=[int64]$runResult.in_domain_presented_event_count;$outDomain+=[int64]$runResult.out_of_domain_presented_event_count
    $missing+=[int64]$runResult.missing_mapping_count;$ambiguous+=[int64]$runResult.ambiguous_mapping_count
    $duplicate+=[int64]$runResult.duplicate_physical_ordinal_count
    $upstreamInvalid+=[int64]$runResult.upstream_candidate_authority_invalid_count
    $observedPresented+=[int64]$runResult.formal_population_authority.observed_presented_count
    $nonformalObserved+=[int64]$runResult.formal_population_authority.nonformal_observed_presented_count
    $invalidNonformal+=[int64]$runResult.formal_population_authority.upstream_invalid_nonformal_count
    $exactNonformal+=[int64]$runResult.formal_population_authority.upstream_exact_nonformal_count
    $observedMapped+=[int64]$runResult.observed_physical_mapping_diagnostic.mapped_exact_count
    $observedMissing+=[int64]$runResult.observed_physical_mapping_diagnostic.missing_mapping_count
    $observedAmbiguous+=[int64]$runResult.observed_physical_mapping_diagnostic.ambiguous_mapping_count
    $observedDuplicate+=[int64]$runResult.observed_physical_mapping_diagnostic.duplicate_physical_ordinal_count
}
$result=[ordered]@{
    schema='mvm-p2-d5-2-w2-c1-displayed-physical-mapping-2';stage='P2-D5-2-W2-C1.3'
    source_c011_directory=(Resolve-Path -LiteralPath $C011Directory).Path
    source_upstream_inventory_proof=(Resolve-Path -LiteralPath $upstreamProof).Path
    mapping_rule=$script:MvmDisplayedMappingRule
    mapping_support='CLOSED_PREDECESSOR_TO_SUCCESSOR'
    domain_membership_evaluated_after_mapping=$true
    shadow_only=$true;performance_accounting_connected=$false;intent_satisfaction_connected=$false
    frame_swapped_formal_authority_changed=$false;abi_version=4
    run_count=$runCount;presented_candidate_count=$presented;mapped_exact_count=$mapped
    observed_presented_count=$observedPresented;formal_presented_count=$presented
    nonformal_observed_presented_count=$nonformalObserved
    upstream_invalid_nonformal_count=$invalidNonformal;upstream_exact_nonformal_count=$exactNonformal
    observed_physical_mapped_exact_count=$observedMapped
    observed_physical_missing_count=$observedMissing;observed_physical_ambiguous_count=$observedAmbiguous
    observed_physical_duplicate_ordinal_count=$observedDuplicate
    formal_population_authority='B2_TERMINAL_FINAL_STATE_PRESENTED_EXACT_EVENT_SET'
    formal_population_authority_valid=@($runResults|Where-Object{-not[bool]$_.formal_population_authority.authority_valid}).Count-eq0
    in_domain_presented_event_count=$inDomain;out_of_domain_presented_event_count=$outDomain
    missing_mapping_count=$missing;ambiguous_mapping_count=$ambiguous
    duplicate_physical_ordinal_count=$duplicate
    upstream_candidate_authority_invalid_count=$upstreamInvalid
    upstream_inventory_proof_sha256=((Get-FileHash -LiteralPath $upstreamProof -Algorithm SHA256).Hash.ToLowerInvariant())
    mapping_exact=$globalBlockerList.Count-eq0;blockers=$globalBlockerList;runs=$runResults
    verdict=$(if($globalBlockerList.Count-eq0){'DISPLAYED_QPC_PHYSICAL_VBLANK_ORDINAL_EXACT'}else{'DISPLAYED_QPC_PHYSICAL_VBLANK_ORDINAL_INVALID'})
}
$result|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
if($globalBlockerList.Count-ne0){Fail "W2-C1 mapping authorityが不成立です: $($globalBlockerList-join', ')"}
Write-Host "P2-D5-2 W2-C1 mapping: PASS ($runCount/$runCount) $($result.verdict)"
