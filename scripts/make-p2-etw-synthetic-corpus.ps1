[CmdletBinding()]
param(
    [string]$SourceArtifactDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'bench\results\f3-b0.6-r2-20260822-1919'),
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$OracleChecker = (Join-Path $PSScriptRoot 'check-p2-etw-present-history.ps1'),
    [string]$VBlankChecker = (Join-Path $PSScriptRoot 'check-p2-vblank-shadow.ps1')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$expected = [ordered]@{
    manifest = '60ec6a04d584aaa87bb5469a6dc45dbeff65490c2677d5630c34d4c9cafed88f'
    app = '9cfc6022b3d426c78250d990926d4b5572b118e98977c60490f33dfbf81fe9cc'
    etw = '4abc197c2a4c77ecf7aa8bf82c31e80abf3945c89f34234c49fcc0c60ba03535'
}

function Hash([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Choose([int]$N, [int]$K) {
    if ($K -lt 0 -or $K -gt $N) { return [int64]0 }
    $K = [Math]::Min($K, $N - $K)
    [int64]$value = 1
    for ($i = 1; $i -le $K; ++$i) { $value = [int64](($value * ($N - $K + $i)) / $i) }
    return $value
}

$SourceArtifactDirectory = (Resolve-Path -LiteralPath $SourceArtifactDirectory).Path
$appPath = Join-Path $SourceArtifactDirectory 'traced-app.json'
$etwPath = Join-Path $SourceArtifactDirectory 'present-history-raw.json'
$manifestPath = Join-Path $SourceArtifactDirectory 'manifest.sha256'
foreach ($path in @($appPath, $etwPath, $manifestPath, $OracleChecker, $VBlankChecker)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "R3必須fileがありません: $path" }
}
$actualHashes = [ordered]@{ manifest=Hash $manifestPath; app=Hash $appPath; etw=Hash $etwPath }
foreach ($name in $expected.Keys) {
    if ($actualHashes[$name] -ne $expected[$name]) {
        throw "固定15秒artifactのhashが不一致です: $name actual=$($actualHashes[$name])"
    }
}
if (Test-Path -LiteralPath $OutputDirectory) { throw "既存corpusを上書きしません: $OutputDirectory" }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$casesDirectory = Join-Path $OutputDirectory 'cases'
New-Item -ItemType Directory -Path $casesDirectory | Out-Null

$sourceOraclePath = Join-Path $OutputDirectory 'source-oracle.json'
& pwsh -NoProfile -File $OracleChecker -AppJson $appPath -EtwJson $etwPath `
    -Output $sourceOraclePath -VBlankChecker $VBlankChecker
if ($LASTEXITCODE -ne 0) { throw '固定15秒artifactのoracle再検査に失敗しました' }
$app = Get-Content -LiteralPath $appPath -Raw -Encoding utf8 | ConvertFrom-Json
$oracle = Get-Content -LiteralPath $sourceOraclePath -Raw -Encoding utf8 | ConvertFrom-Json
if ($oracle.oracle_status -ne 'ORACLE_VALID' -or $oracle.collision_evidence_status -ne
    'COLLISION_NOT_OBSERVED' -or $oracle.app_swap_count -ne 900 -or $oracle.etw_present_count -ne 900) {
    throw '固定15秒artifactはR3 source契約を満たしません'
}
$samples = @($app.presentation_opportunity.physical_vblank.samples)
$swaps = @($app.presentation_opportunity.swap_records)
$records = @($oracle.records)
$sampleIndexByOrdinal = @{}
for ($i = 0; $i -lt $samples.Count; ++$i) {
    $sampleIndexByOrdinal[[string][int64]$samples[$i].ordinal] = $i
}

$caseIndex = [System.Collections.Generic.List[object]]::new()
$usedStarts = @{}
function Position([int]$Start, [int]$Width) {
    if ($Start -le 3) { return 'MEASUREMENT_START' }
    if ($Start + $Width -ge $records.Count - 3) { return 'MEASUREMENT_END' }
    if ([Math]::Abs($Start - [int]($records.Count / 2)) -le 3) { return 'MEASUREMENT_MIDDLE' }
    return 'SWEEP'
}
function Add-Case([string]$Family, [int]$Start, [int]$RecordCount,
                  [int]$OpportunityCount, [int]$TargetExtra) {
    $sourceRecords = @($records[$Start..($Start + $RecordCount - 1)])
    for ($j = 1; $j -lt $sourceRecords.Count; ++$j) {
        if ([int64]$sourceRecords[$j].first_opportunity_ordinal -ne
            [int64]$sourceRecords[$j - 1].first_opportunity_ordinal + 1) { return $false }
    }
    $firstActual = [int64]$sourceRecords[0].first_opportunity_ordinal
    $lastActual = [int64]$sourceRecords[-1].first_opportunity_ordinal
    if ($OpportunityCount -eq $RecordCount) {
        $firstOpportunity = $firstActual
    } elseif ($OpportunityCount -gt $RecordCount) {
        $firstOpportunity = $firstActual
    } else {
        $firstOpportunity = $lastActual - $OpportunityCount + 1
    }
    $targetBracket = $firstOpportunity + $OpportunityCount - 1 + $TargetExtra
    $sliceFirst = $firstOpportunity
    $sliceLastBoundary = $targetBracket + 1
    if (-not $sampleIndexByOrdinal.ContainsKey([string]$sliceFirst) -or
        -not $sampleIndexByOrdinal.ContainsKey([string]$sliceLastBoundary)) { return $false }
    $sliceStartIndex = [int]$sampleIndexByOrdinal[[string]$sliceFirst]
    $sliceEndIndex = [int]$sampleIndexByOrdinal[[string]$sliceLastBoundary]
    $slice = @($samples[$sliceStartIndex..$sliceEndIndex])
    $targetIndex = [int]$sampleIndexByOrdinal[[string]$targetBracket]
    $lower = [int64]$samples[$targetIndex].qpc
    $upper = [int64]$samples[$targetIndex + 1].qpc
    $maxOriginal = [int64]($swaps[$Start..($Start + $RecordCount - 1)] |
        Measure-Object swap_qpc -Maximum).Maximum
    $syntheticStart = [Math]::Max($lower, $maxOriginal) + 1
    if ($syntheticStart + $RecordCount - 1 -ge $upper) { return $false }

    $solutionCount = Choose $OpportunityCount $RecordCount
    $solutionClass = if ($solutionCount -eq 0) { 'NO_SOLUTION' } elseif ($solutionCount -eq 1) {
        'UNIQUE'
    } else { 'AMBIGUOUS' }
    $caseId = '{0}-{1:D4}-{2}' -f $Family.ToLowerInvariant(), $Start,
        $caseIndex.Count.ToString('D3')
    $caseDirectory = Join-Path $casesDirectory $caseId
    New-Item -ItemType Directory -Path $caseDirectory | Out-Null
    $mapperRecords = @()
    $hiddenRecords = @()
    for ($j = 0; $j -lt $RecordCount; ++$j) {
        $submission = $Start + $j
        $original = [int64]$swaps[$submission].swap_qpc
        $synthetic = $syntheticStart + $j
        $mapperRecords += [ordered]@{
            submission_index=$submission; original_callback_qpc=$original
            synthetic_callback_qpc=$synthetic; synthetic_delay_ticks=$synthetic-$original
            synthetic_callback_bracket=$targetBracket
        }
        $hiddenRecords += [ordered]@{
            submission_index=$submission; status=[string]$sourceRecords[$j].status
            present_ids=@($sourceRecords[$j].present_ids)
            displayed=@($sourceRecords[$j].displayed)
            actual_physical_opportunity_ordinal=[int64]$sourceRecords[$j].first_opportunity_ordinal
        }
    }
    $mapperSamples = @()
    for ($j = 0; $j -lt $slice.Count; ++$j) {
        $mapperSamples += [ordered]@{
            source_sample_index=$sliceStartIndex+$j
            ordinal=[int64]$slice[$j].ordinal; qpc=[int64]$slice[$j].qpc
        }
    }
    $mapperInput = [ordered]@{
        schema='mvm-p2-r3-mapper-input-1'; case_id=$caseId; sync_interval=1
        qpc_frequency=[int64]$app.presentation_opportunity.qpc_frequency
        source_app_sha256=$actualHashes.app; source_etw_sha256=$actualHashes.etw
        construction_model='STRICT_MONOTONE_INJECTIVE_OVER_VISIBLE_OPPORTUNITIES'
        mapper_visible_opportunity_count=$OpportunityCount
        vblank_samples=$mapperSamples; records=$mapperRecords
    }
    $hiddenOracle = [ordered]@{
        schema='mvm-p2-r3-hidden-oracle-1'; case_id=$caseId
        expected_solution_class=$solutionClass; expected_solution_count=$solutionCount
        family=$Family; position=Position $Start $RecordCount
        source_record_start=$Start; source_record_count=$RecordCount
        source_vblank_start_index=$sliceStartIndex; source_vblank_sample_count=$slice.Count
        target_synthetic_bracket=$targetBracket
        oracle_fields_excluded_from_mapper_input=$true
        records=$hiddenRecords
    }
    $mapperPath = Join-Path $caseDirectory 'mapper-input.json'
    $hiddenPath = Join-Path $caseDirectory 'hidden-oracle.json'
    $mapperInput | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $mapperPath -Encoding utf8
    $hiddenOracle | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $hiddenPath -Encoding utf8
    $caseIndex.Add([ordered]@{
        case_id=$caseId; family=$Family; position=$hiddenOracle.position
        expected_solution_class=$solutionClass; record_count=$RecordCount
        opportunity_count=$OpportunityCount
        mapper_input="cases/$caseId/mapper-input.json"
        hidden_oracle="cases/$caseId/hidden-oracle.json"
        mapper_input_sha256=Hash $mapperPath; hidden_oracle_sha256=Hash $hiddenPath
    })
    return $true
}

function Fill-Family([string]$Family, [int]$Wanted, [int]$RecordCount,
                     [int]$OpportunityCount, [int]$TargetExtra) {
    $before = $caseIndex.Count
    $preferred = @(0,1,2,447,448,449,($records.Count-$RecordCount-2),
        ($records.Count-$RecordCount-1),($records.Count-$RecordCount))
    $step = [Math]::Max(1, [int](($records.Count - $RecordCount) / ($Wanted + 1)))
    for ($i = $step; $i -le $records.Count - $RecordCount; $i += $step) { $preferred += $i }
    for ($i = 0; $i -le $records.Count - $RecordCount; ++$i) { $preferred += $i }
    foreach ($start in @($preferred | Select-Object -Unique)) {
        $key = "$Family/$start"
        if ($usedStarts.ContainsKey($key)) { continue }
        if (Add-Case $Family $start $RecordCount $OpportunityCount $TargetExtra) {
            $usedStarts[$key] = $true
        }
        if ($caseIndex.Count - $before -ge $Wanted) { break }
    }
    if ($caseIndex.Count - $before -lt $Wanted) {
        throw "$Family fixtureを$Wanted件生成できませんでした"
    }
}

Fill-Family 'PAIR_UNIQUE' 24 2 2 0
Fill-Family 'TRIPLE_UNIQUE' 12 3 3 0
Fill-Family 'ONE_BRACKET_LATE_AMBIGUOUS' 8 2 3 0
Fill-Family 'NO_SOLUTION_CAPACITY' 8 3 2 0

$actualGapCount = 0
for ($i = 1; $i -lt $records.Count; ++$i) {
    if ([int64]$records[$i].first_opportunity_ordinal -gt
        [int64]$records[$i - 1].first_opportunity_ordinal + 1) { ++$actualGapCount }
}
$presentIdAvailable = @($records | Where-Object { @($_.present_ids).Count -gt 0 }).Count
$counts = [ordered]@{}
foreach ($entry in $caseIndex) {
    $key = [string]$entry.expected_solution_class
    if (-not $counts.Contains($key)) { $counts[$key] = 0 }
    ++$counts[$key]
}
$index = [ordered]@{
    schema='mvm-p2-r3-synthetic-collision-corpus-1'
    authority='diagnostic_only'; corpus_status='GENERATED_NOT_YET_CHECKED'
    mapper_proof_status='NOT_YET_EVALUABLE'; mapper_changed=$false
    source_artifact='bench/results/f3-b0.6-r2-20260822-1919'
    source_hashes=$actualHashes
    source_oracle_sha256=Hash $sourceOraclePath
    source_oracle_status=$oracle.oracle_status
    source_collision_status=$oracle.collision_evidence_status
    source_record_count=$records.Count
    source_displayed_count=@($records | Where-Object status -eq DISPLAYED).Count
    source_present_ids_available_count=$presentIdAvailable
    source_actual_gap_count=$actualGapCount
    actual_gap_cases_generated=0
    actual_gap_note=if ($actualGapCount -eq 0) { 'source oracleにactual presentation gapは無い' } else { '' }
    construction_model='strict monotone + injective + order preserving over each visible VBlank slice'
    case_count=$caseIndex.Count; solution_class_counts=$counts; cases=$caseIndex
}
$indexPath = Join-Path $OutputDirectory 'corpus-index.json'
$index | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $indexPath -Encoding utf8
$manifest = Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File | Sort-Object FullName |
    ForEach-Object {
        $relative=$_.FullName.Substring($OutputDirectory.Length+1).Replace('\','/')
        "$(Hash $_.FullName)  $relative"
    }
$manifest | Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.sha256') -Encoding ascii
Write-Host "R3 synthetic collision corpus: GENERATED ($($caseIndex.Count) cases)"
