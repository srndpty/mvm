[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$C011Directory,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$Inventory=(Join-Path $PSScriptRoot 'inventory-p2-d5-2-w2-c0-display-candidates.ps1'),
    [string]$PhysicalChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2a-physical-domain.ps1'),
    [string]$EnvelopeChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c01-capture-envelope.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Need($Object,[string]$Name){
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){Fail "必須fieldがありません: $Name"}
    return $Object.$Name
}
foreach($path in @($C011Directory,$Inventory,$PhysicalChecker,$EnvelopeChecker)){
    if(-not(Test-Path -LiteralPath $path)){Fail "W2-C1必須pathがありません: $path"}
}
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c1-mapping-core.ps1')
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
    foreach($path in @($appPath,$etwPath)){if(-not(Test-Path -LiteralPath $path)){Fail "run $run artifactがありません: $path"}}
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
    $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
    $etw=Get-Content -LiteralPath $etwPath -Raw -Encoding utf8|ConvertFrom-Json
    $opportunity=Need $app 'presentation_opportunity';$physical=Need $opportunity 'physical_vblank'
    $shadow=Need $opportunity 'physical_vblank_domain_shadow'
    $scope=Need (Need $app 'native_present_hook') 'intent_scope_provenance'
    $runInventory=$inventoryProof.runs[$run-1]
    $supportCandidates=@($runInventory.candidates|Where-Object{
        [string]$_.display_relation-eq'WITHIN_PREDECESSOR_SUCCESSOR_ENVELOPE'
    })
    foreach($candidate in $supportCandidates){
        if(-not[bool](Need $candidate 'native_exact')-or-not[bool](Need $candidate 'intent_exact')-or
           -not[bool](Need $candidate 'intent_scope_exact')){Fail "run $run upstream candidate authorityが不成立です"}
    }
    $upstreamValid=$physicalValid-and$envelopeValid-and[bool](Need $runInventory 'coverage_complete')-and
        [bool](Need $runInventory 'intent_scope_exact')-and[bool](Need $scope 'authority_pass')
    $mapping=Invoke-MvmDisplayedQpcPhysicalMapping -Candidates $supportCandidates `
        -Samples @(Need $physical 'samples') `
        -PredecessorOrdinal ([int64](Need $shadow 'predecessor_ordinal')) `
        -SuccessorOrdinal ([int64](Need $shadow 'successor_ordinal')) `
        -OriginOrdinal ([int64](Need $shadow 'origin_ordinal')) `
        -LastOrdinal ([int64](Need $shadow 'last_ordinal')) `
        -PhysicalAuthorityValid $upstreamValid `
        -EtwEventsLost ([int64](Need $etw 'etw_events_lost')) `
        -EtwBuffersLost ([int64](Need $etw 'etw_buffers_lost')) `
        -PresentEventOverflowCount ([int64](Need $etw 'present_event_overflow_count'))
    foreach($blocker in @($mapping.blockers)){$globalBlockers[[string]$blocker]=$true}
    $mapping.run=$run
    $mapping.upstream_authority_valid=$upstreamValid
    $mapping.capture_envelope_valid=$envelopeValid
    $mapping.physical_authority_valid=$physicalValid
    $mapping.intent_scope_exact=[bool]$runInventory.intent_scope_exact
    $runResults+=,$mapping
}
$globalBlockerList=@($globalBlockers.Keys|Sort-Object)
$presented=0L;$mapped=0L;$inDomain=0L;$outDomain=0L;$missing=0L;$ambiguous=0L;$duplicate=0L
foreach($runResult in $runResults){
    $presented+=[int64]$runResult.presented_candidate_count;$mapped+=[int64]$runResult.mapped_exact_count
    $inDomain+=[int64]$runResult.in_domain_presented_event_count;$outDomain+=[int64]$runResult.out_of_domain_presented_event_count
    $missing+=[int64]$runResult.missing_mapping_count;$ambiguous+=[int64]$runResult.ambiguous_mapping_count
    $duplicate+=[int64]$runResult.duplicate_physical_ordinal_count
}
$result=[ordered]@{
    schema='mvm-p2-d5-2-w2-c1-displayed-physical-mapping-1';stage='P2-D5-2-W2-C1'
    source_c011_directory=(Resolve-Path -LiteralPath $C011Directory).Path
    mapping_rule=$script:MvmDisplayedMappingRule
    mapping_support='CLOSED_PREDECESSOR_TO_SUCCESSOR'
    domain_membership_evaluated_after_mapping=$true
    shadow_only=$true;performance_accounting_connected=$false;intent_satisfaction_connected=$false
    frame_swapped_formal_authority_changed=$false;abi_version=4
    run_count=$runCount;presented_candidate_count=$presented;mapped_exact_count=$mapped
    in_domain_presented_event_count=$inDomain;out_of_domain_presented_event_count=$outDomain
    missing_mapping_count=$missing;ambiguous_mapping_count=$ambiguous
    duplicate_physical_ordinal_count=$duplicate
    mapping_exact=$globalBlockerList.Count-eq0;blockers=$globalBlockerList;runs=$runResults
    verdict=$(if($globalBlockerList.Count-eq0){'DISPLAYED_QPC_PHYSICAL_VBLANK_ORDINAL_EXACT'}else{'DISPLAYED_QPC_PHYSICAL_VBLANK_ORDINAL_INVALID'})
}
$result|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
if($globalBlockerList.Count-ne0){Fail "W2-C1 mapping authorityが不成立です: $($globalBlockerList-join', ')"}
Write-Host "P2-D5-2 W2-C1 mapping: PASS ($runCount/$runCount) $($result.verdict)"
