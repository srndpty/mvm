$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$buffer = Get-Content -Raw (Join-Path $repoRoot "src/media/gpu_preview/source_frame_buffer.cpp")
$pairer = Get-Content -Raw (Join-Path $repoRoot "src/media/gpu_preview/exact_frame_pairer.cpp")
$worker = Get-Content -Raw (Join-Path $repoRoot "src/media/gpu_preview/source_decode_worker.cpp")
$engine = Get-Content -Raw (Join-Path $repoRoot "src/preview_engine/preview_engine.cpp")

$checks = @(
    @{ Name = "buffer envelope"; Pass = $buffer.Contains("submitFrameForOutput") },
    @{ Name = "pairer output authority"; Pass = $pairer.Contains("peekFrontOutputFrameNumber") -and $pairer.Contains("takeExactEnvelopes") },
    @{ Name = "decoded identity preserved"; Pass = $worker.Contains("submitFrameForOutput(frame, outputAnchor + delta)") },
    @{ Name = "explicit source request"; Pass = $engine -match '(?s)requestSeek\(\s*sourceFrame,\s*target\.outputFrame' },
    @{ Name = "presentation remains output identity"; Pass = $engine.Contains("status.position = {target}") },
    @{ Name = "forbidden decoded overwrite absent"; Pass = -not $worker.Contains("frame.frameNumber = ticket.outputFrameNumber") }
)

$failed = @($checks | Where-Object { -not $_.Pass })
foreach ($check in $checks) {
    $state = if ($check.Pass) { "PASS" } else { "FAIL" }
    Write-Host "$state $($check.Name)"
}
if ($checks.Count -eq 0 -or $failed.Count -ne 0) { exit 1 }
exit 0
