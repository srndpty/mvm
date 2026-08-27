[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$C011Directory,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Need($Object,[string]$Name){
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){Fail "C2.4必須fieldがありません: $Name"}
    return $Object.$Name
}
$summaryPath=Join-Path $C011Directory 'w2-c01-live-summary.json'
if(-not(Test-Path -LiteralPath $summaryPath)){Fail "C2.4 source summaryがありません: $summaryPath"}
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c24-formal-transport-core.ps1')
$summary=Get-Content -LiteralPath $summaryPath -Raw -Encoding utf8|ConvertFrom-Json
$runCount=[int](Need $summary 'runs');if($runCount-le0){Fail 'C2.4 run countが不正です'}
$runs=@();$globalBlockers=@{};$duplicate=0L;$outside=0L;$eligible=0L;$records=0L
for($run=1;$run-le$runCount;++$run){
    $appPath=Join-Path $C011Directory "run-$run\traced-app.json"
    if(-not(Test-Path -LiteralPath $appPath)){Fail "C2.4 traced appがありません: $appPath"}
    $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
    $scope=Need (Need $app 'native_present_hook') 'intent_scope_provenance'
    $replayed=Invoke-MvmC24FormalTransportPolicy -ProducerRecords @(Need $scope 'records') `
        -RecordedDuplicateSuppressedCount ([int64](Need $scope 'duplicate_transport_suppressed_count')) `
        -RecordedOutsideSuppressedCount ([int64](Need $scope 'outside_required_transport_suppressed_count'))
    $replayed.run=$run
    $replayed.sealed_traced_app_sha256=(Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash.ToLowerInvariant()
    foreach($blocker in @($replayed.blockers)){$globalBlockers[[string]$blocker]=$true}
    $duplicate+=[int64]$replayed.checker_derived_duplicate_suppressed_count
    $outside+=[int64]$replayed.checker_derived_outside_suppressed_count
    $eligible+=[int64]$replayed.checker_derived_transport_eligible_count
    $records+=[int64]$replayed.producer_record_count
    $runs+=,$replayed
}
$blockerList=@($globalBlockers.Keys|Sort-Object)
$result=[ordered]@{
    schema='mvm-p2-d5-2-w2-c24-formal-transport-policy-1';stage='P2-D5-2-W2-C2.4'
    source_c011_directory=(Resolve-Path -LiteralPath $C011Directory).Path
    source_summary_sha256=(Get-FileHash -LiteralPath $summaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
    policy_authority='CHECKER_DERIVED_FROM_PRODUCER_SEMANTICS'
    run_count=$runCount;producer_record_count=$records
    checker_derived_transport_eligible_count=$eligible
    checker_derived_duplicate_suppressed_count=$duplicate
    checker_derived_outside_suppressed_count=$outside
    policy_exact=$blockerList.Count-eq0;blockers=$blockerList;runs=$runs
    verdict=$(if($blockerList.Count-eq0){'FORMAL_TRANSPORT_POLICY_EXACT'}else{'FORMAL_TRANSPORT_POLICY_INVALID'})
}
$outputDirectory=Split-Path -Parent $Output
if(-not[string]::IsNullOrWhiteSpace($outputDirectory)-and-not(Test-Path -LiteralPath $outputDirectory)){
    New-Item -ItemType Directory -Path $outputDirectory|Out-Null
}
$result|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
if(-not[bool]$result.policy_exact){Fail "C2.4 formal transport policyが不成立です: $($blockerList-join', ')"}
Write-Output "P2-D5-2 W2-C2.4 checker: PASS ($runCount/$runCount)"
