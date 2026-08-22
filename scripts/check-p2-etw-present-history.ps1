param(
    [Parameter(Mandatory=$true)][string]$AppJson,
    [Parameter(Mandatory=$true)][string]$EtwJson,
    [Parameter(Mandatory=$true)][string]$Output,
    [Parameter(Mandatory=$true)][string]$VBlankChecker,
    [int]$ProcessExitCode = 0
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$Message) { throw $Message }
function Need($Object, [string]$Name) {
    if ($null -eq $Object -or $Object.PSObject.Properties.Name -notcontains $Name) {
        Fail "必須fieldがありません: $Name"
    }
    return $Object.$Name
}
function Equal($Actual, $Expected, [string]$Name) {
    if ($Actual -ne $Expected) { Fail "$Name が一致しません (expected=$Expected actual=$Actual)" }
}

if ($ProcessExitCode -ne 0) { Fail "application processが失敗しました: $ProcessExitCode" }
& pwsh -NoProfile -File $VBlankChecker -Json $AppJson -ProcessExitCode $ProcessExitCode *> $null
if ($LASTEXITCODE -ne 0) { Fail 'physical VBlank shadow authorityがINVALIDです' }

$app = Get-Content -Raw -LiteralPath $AppJson | ConvertFrom-Json
$etw = Get-Content -Raw -LiteralPath $EtwJson | ConvertFrom-Json
Equal (Need $etw 'schema') 'mvm-p2-etw-present-history-1' 'ETW schema'
Equal ([bool](Need $etw 'raw_displayed_qpc')) $true 'raw_displayed_qpc'
Equal ([int64](Need $etw 'etw_events_lost')) 0 'etw_events_lost'
Equal ([int64](Need $etw 'etw_buffers_lost')) 0 'etw_buffers_lost'
Equal ([int64](Need $etw 'present_event_overflow_count')) 0 'present_event_overflow_count'

$opportunity = Need $app 'presentation_opportunity'
$measurementStart = [int64](Need $opportunity 'measurement_start_qpc')
$measurementEnd = [int64](Need $opportunity 'measurement_end_qpc_exclusive')
if ($measurementStart -le 0 -or $measurementEnd -le $measurementStart) {
    Fail 'application measurement windowが不正です'
}
$qpcFrequency = [int64](Need $opportunity 'qpc_frequency')
Equal ([int64](Need $etw 'qpc_frequency')) $qpcFrequency 'ETW/app qpc_frequency'
$swaps = @(Need $opportunity 'swap_records')
Equal $swaps.Count ([int](Need $opportunity 'swap_record_count')) 'app swap count'
if ($swaps.Count -lt 2) { Fail 'app swap sequenceが不足しています' }
for ($index = 0; $index -lt $swaps.Count; ++$index) {
    Equal ([int64](Need $swaps[$index] 'swap_ordinal')) $index "swap_ordinal[$index]"
    if ($index -gt 0 -and [int64]$swaps[$index].swap_qpc -le [int64]$swaps[$index - 1].swap_qpc) {
        Fail "app swap QPCの順序が不正です: $index"
    }
}

$targetPid = [int64](Need $etw 'target_process_id')
$windowEvents = @(Need $etw 'events' | Where-Object {
    [int64]$_.process_id -eq $targetPid -and
    [int64]$_.present_start_qpc -ge $measurementStart -and
    [int64]$_.present_start_qpc -lt $measurementEnd
})
$groups = @($windowEvents | Group-Object {
    "$(Need $_ 'swap_chain_address')|$(Need $_ 'window_handle')"
})
$candidates = @($groups | Where-Object {
    $_.Count -eq $swaps.Count -and @($_.Group | Where-Object { [int64]$_.sync_interval -ne 1 }).Count -eq 0
})
if ($candidates.Count -ne 1) {
    Fail "target swapchainを一意に選べません (candidate_count=$($candidates.Count))"
}
$targetSwapChain = [string](Need $candidates[0].Group[0] 'swap_chain_address')
$targetWindow = [string](Need $candidates[0].Group[0] 'window_handle')
$presents = @($candidates[0].Group | Sort-Object {[int64]$_.present_start_qpc})
Equal $presents.Count $swaps.Count 'ETW Present/app swap exact count'
$sequenceBase = [int64](Need $presents[0] 'sequence_index')
for ($index = 0; $index -lt $presents.Count; ++$index) {
    $present = $presents[$index]
    Equal ([int64](Need $present 'sequence_index')) ($sequenceBase + $index) `
        "ETW sequence_index[$index]"
    Equal ([int64](Need $present 'sync_interval')) 1 "SyncInterval[$index]"
    if ($index -gt 0 -and [int64]$present.present_start_qpc -le
                            [int64]$presents[$index - 1].present_start_qpc) {
        Fail "ETW Present orderがstrictではありません: $index"
    }
    if ([int64]$present.present_start_qpc -gt [int64]$swaps[$index].swap_qpc) {
        Fail "sequence joinでPresentStartが対応swapより後です: $index"
    }
}

$vblank = Need (Need $opportunity 'physical_vblank') 'samples'
$samples = @($vblank)
if ($samples.Count -lt 120) { Fail "phase alignment用VBlankが120件未満です: $($samples.Count)" }
$identity = Need (Need $opportunity 'physical_vblank') 'window_output_start'
$refreshNumerator = [int64](Need $identity 'refresh_numerator')
$refreshDenominator = [int64](Need $identity 'refresh_denominator')
$periodScaled = [decimal]$qpcFrequency * [decimal]$refreshDenominator
$phaseCandidates = @()
for ($index = 0; $index -lt 120; ++$index) {
    $phaseCandidates += ([decimal][int64]$samples[$index].qpc * [decimal]$refreshNumerator) -
        ([decimal][int64]$samples[$index].ordinal * $periodScaled)
}
$phaseCandidates = @($phaseCandidates | Sort-Object)
$originScaled = [decimal]$phaseCandidates[59]

function Map-DisplayedQpc([int64]$Qpc) {
    $relative = ([decimal]$Qpc * [decimal]$refreshNumerator) - $originScaled
    $floorOrdinal = [decimal]::Floor($relative / $periodScaled)
    $remainder = $relative - $floorOrdinal * $periodScaled
    if ($remainder * 2 -eq $periodScaled) { Fail "DisplayedQPCがVBlank境界で曖昧です: $Qpc" }
    $ordinal = if ($remainder * 2 -lt $periodScaled) { $floorOrdinal } else { $floorOrdinal + 1 }
    $residual = $relative - $ordinal * $periodScaled
    $firstOrdinal = [decimal][int64]$samples[0].ordinal
    $lastOrdinal = [decimal][int64]$samples[-1].ordinal
    if ($ordinal -lt $firstOrdinal -or $ordinal -gt $lastOrdinal) {
        Fail "DisplayedQPCがphysical VBlank観測範囲外です: $Qpc"
    }
    return [ordered]@{
        opportunity_ordinal = [int64]$ordinal
        residual_scaled_numerator = [int64]$residual
        residual_scaled_denominator = $refreshNumerator
    }
}

function Bracket-Swap([int64]$Qpc) {
    for ($index = 0; $index + 1 -lt $samples.Count; ++$index) {
        if ([int64]$samples[$index].qpc -le $Qpc -and $Qpc -lt [int64]$samples[$index + 1].qpc) {
            return [int64]$samples[$index].ordinal
        }
    }
    Fail "swapをphysical VBlankへbracketできません: $Qpc"
}

$records = @()
$sameBracketResolved = 0
$previousSwapBracket = $null
$previousOracleIdentity = $null
for ($index = 0; $index -lt $presents.Count; ++$index) {
    $displayed = @()
    foreach ($display in @(Need $presents[$index] 'displayed')) {
        $mapped = Map-DisplayedQpc ([int64](Need $display 'qpc'))
        $displayed += [ordered]@{
            frame_type = [string](Need $display 'frame_type')
            qpc = [int64]$display.qpc
            opportunity_ordinal = $mapped.opportunity_ordinal
            residual_scaled_numerator = $mapped.residual_scaled_numerator
            residual_scaled_denominator = $mapped.residual_scaled_denominator
        }
    }
    $status = if ($displayed.Count -eq 0) { 'UNPRESENTED' } else { 'DISPLAYED' }
    $firstOpportunity = if ($displayed.Count -eq 0) { $null } else { $displayed[0].opportunity_ordinal }
    $swapBracket = Bracket-Swap ([int64]$swaps[$index].swap_qpc)
    $identityValue = if ($status -eq 'UNPRESENTED') { 'UNPRESENTED' } else { "MAPPED:$firstOpportunity" }
    if ($null -ne $previousSwapBracket -and $swapBracket -eq $previousSwapBracket -and
        $identityValue -ne $previousOracleIdentity) {
        ++$sameBracketResolved
    }
    $previousSwapBracket = $swapBracket
    $previousOracleIdentity = $identityValue
    $records += [ordered]@{
        sequence_index = $index
        swap_ordinal = [int64]$swaps[$index].swap_ordinal
        swap_qpc = [int64]$swaps[$index].swap_qpc
        source_frame_identity = [int64]$swaps[$index].presented_output_frame
        present_start_qpc = [int64]$presents[$index].present_start_qpc
        swap_chain_address = $targetSwapChain
        sync_interval = [int64]$presents[$index].sync_interval
        present_flags = [int64]$presents[$index].present_flags
        present_ids = @(Need $presents[$index] 'present_ids')
        status = $status
        first_opportunity_ordinal = $firstOpportunity
        physical_repeat_count = [Math]::Max(0, $displayed.Count - 1)
        displayed = $displayed
        frame_swapped_bracket_ordinal = $swapBracket
    }
}

$collisionMode = [string](Need $etw 'collision_evidence_mode')
$collisionStatus = if ($sameBracketResolved -gt 0) { 'COLLISION_RESOLVED' } else {
    'COLLISION_NOT_OBSERVED'
}

$result = [ordered]@{
    schema = 'mvm-p2-etw-present-history-oracle-1'
    authority = 'diagnostic_only'
    oracle_status = 'ORACLE_VALID'
    mapper_proof_status = 'NOT_YET_EVALUABLE'
    mapper_changed = $false
    target_process_id = $targetPid
    target_swap_chain_address = $targetSwapChain
    target_window_handle = $targetWindow
    measurement_start_qpc = $measurementStart
    measurement_end_qpc_exclusive = $measurementEnd
    app_swap_count = $swaps.Count
    etw_present_count = $presents.Count
    sequence_order_mismatch_count = 0
    etw_events_lost = 0
    phase_alignment = [ordered]@{
        calibration_vblank_count = 120
        origin_scaled = [string]$originScaled
        period_scaled = [string]$periodScaled
        refresh_numerator = $refreshNumerator
        refresh_denominator = $refreshDenominator
    }
    same_bracket_identity_resolved_count = $sameBracketResolved
    collision_evidence_status = $collisionStatus
    r2_exit_status = if ($sameBracketResolved -gt 0) { 'PASS' } else { 'PENDING_SYNTHETIC_CORPUS' }
    collision_evidence_mode = $collisionMode
    cadence_diagnostic = Need $etw 'cadence_diagnostic'
    records = $records
}
$result | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "ETW Present-History oracle: PASS ($($presents.Count)/$($swaps.Count))"
