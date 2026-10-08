<#
.SYNOPSIS
    P4-3 の source と検証を新しい証拠 directory に保存する。
#>
[CmdletBinding()]
param(
    [ValidateSet('Focused', 'Real', 'Regressions', 'BuildIndependent', 'Lint', 'Release')]
    [string]$Stage = 'Focused',
    [string]$EvidenceDirectory,
    [string]$Manim = "$env:APPDATA/uv/tools/manim/Scripts/manim.exe"
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$pwshExe = (Get-Process -Id $PID).Path
$gitTool = Get-Command git -ErrorAction SilentlyContinue
$gitExe = if ($gitTool) { $gitTool.Source } else { 'C:\Program Files\Git\cmd\git.exe' }
if (-not (Test-Path -LiteralPath $gitExe)) { throw 'git がありません' }
if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path $repoRoot ('build/math-p43-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Stage)
}
if (Test-Path -LiteralPath $EvidenceDirectory) { throw '既存の証拠 directory は上書きしません' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$evidenceRoot = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$sources = @(& $gitExe -C $repoRoot ls-files --cached --others --exclude-standard src apps tests scripts cmake CMakeLists.txt)
$hashes = foreach ($relative in $sources) {
    $sourcePath = Join-Path $repoRoot $relative
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) { continue }
    $copyPath = Join-Path $evidenceRoot ('sources/' + $relative)
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $copyPath) -Force
    Copy-Item -LiteralPath $sourcePath -Destination $copyPath
    @{ path = $relative; sha256 = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash }
}
@{ revision = (& $gitExe -C $repoRoot rev-parse HEAD).Trim(); hashes = @($hashes) } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'source-state.json') -Encoding utf8NoBOM
$records = [System.Collections.Generic.List[object]]::new()
function Invoke-Recorded {
    param([string]$Name, [string]$Executable, [string[]]$Arguments)
    $logPath = Join-Path $evidenceRoot ($Name + '.log')
    & $Executable @Arguments 2>&1 | Tee-Object -FilePath $logPath | ForEach-Object { Write-Host $_ }
    $code = $LASTEXITCODE
    $records.Add([ordered]@{ name = $Name; command = $Executable; arguments = $Arguments; exit = $code; log = $logPath })
    $records | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'commands.json') -Encoding utf8NoBOM
    if ($code -ne 0) { throw "$Name の終了コードは $code です" }
}
Push-Location $repoRoot
$displayLease = $null
try {
    Write-Host '【操作可】背面・入力透過の native preview と既存の display-power protocol を使います。'
    if ($Stage -in @('Real', 'Regressions')) {
        . (Join-Path $PSScriptRoot 'lib/test-display-lease.ps1')
        $displayLease = Start-MvmTestDisplayLease
    }
    switch ($Stage) {
        'Focused' {
            Invoke-Recorded 'build' $pwshExe @('scripts/build.ps1', '-Target', 'mvm_test_graph_preview_cache')
            Invoke-Recorded 'cache' 'build/ucrt64-release/bin/mvm_test_graph_preview_cache.exe' @($evidenceRoot)
        }
        'Real' {
            Invoke-Recorded 'build' $pwshExe @('scripts/build.ps1', '-Target', 'mvm_test_graph_native_preview')
            $video = Join-Path $evidenceRoot 'white.mp4'
            Invoke-Recorded 'video-fixture' 'C:\msys64\ucrt64\bin\ffmpeg.exe' @('-hide_banner', '-nostdin', '-f', 'lavfi', '-i', 'color=white:s=320x180:r=30:d=1,geq=lum=235:cb=128:cr=128,setparams=range=limited:colorspace=bt709:color_primaries=bt709:color_trc=bt709', '-c:v', 'libx264', '-qp', '1', '-pix_fmt', 'yuv420p', '-color_range', 'tv', '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709', '-an', $video)
            $decoded = Join-Path $evidenceRoot 'white.yuv'
            Invoke-Recorded 'verify-video-fixture' 'C:\msys64\ucrt64\bin\ffmpeg.exe' @('-v', 'error', '-i', $video, '-pix_fmt', 'yuv420p', '-f', 'rawvideo', $decoded)
            $planes = [System.IO.File]::ReadAllBytes($decoded)
            if ($planes.Length -ne 86400 * 30) { throw '白い動画の復号サイズが違います' }
            for ($offset = 0; $offset -lt $planes.Length; ++$offset) {
                $expectedByte = if ($offset % 86400 -lt 57600) { 235 } else { 128 }
                if ($planes[$offset] -ne $expectedByte) { throw '白い動画が中立 YUV の独立期待値と一致しません' }
            }
            $env:MVM_TEST_FIXED_WINDOW = '1'
            Invoke-Recorded 'native' 'build/ucrt64-release/bin/mvm_test_graph_native_preview.exe' @($Manim, $video, (Join-Path $evidenceRoot 'native'))
        }
        'Regressions' {
            foreach ($target in @('mvm_test_graph_preview_cache', 'mvm_test_math_controller', 'mvm_test_graph_numeric', 'mvm_test_graph_domain', 'mvm_test_graph_render', 'mvm_test_equation_preview_controller', 'mvm_test_timeline_preview_mapping', 'mvm_test_still_layer_compositor', 'mvm_test_clip_effects')) {
                Invoke-Recorded ('build-' + $target) $pwshExe @('scripts/build.ps1', '-Target', $target, '-ReuseConfigure')
            }
            $pattern = '^(graph_.*|math_equation_sequence_preview_controller|math_equation_sequence_native_preview|math_write_native_playback|math_transform_native_playback|m7b_2_timeline_preview_mapping_focused|m7a_1_clip_effects_focused|still_layer_compositor)$'
            $ctest = 'C:\msys64\ucrt64\bin\ctest.exe'
            $selection = (& $ctest --test-dir build/ucrt64-release -N -R $pattern) -join "`n"
            if ($LASTEXITCODE -ne 0 -or $selection -notmatch 'Total Tests: [1-9][0-9]*') { throw '回帰の対象が0件または列挙失敗です' }
            $selection | Set-Content -LiteralPath (Join-Path $evidenceRoot 'selection.log') -Encoding utf8NoBOM
            Invoke-Recorded 'regressions' $ctest @('--test-dir', 'build/ucrt64-release', '-R', $pattern, '--output-on-failure', '--timeout', '120')
        }
        'BuildIndependent' { Invoke-Recorded 'independent' $pwshExe @('scripts/test.ps1', '-Preset', 'ucrt64-release', '-Group', 'BuildIndependent') }
        'Lint' { Invoke-Recorded 'lint' $pwshExe @('scripts/lint.ps1') }
        'Release' { Invoke-Recorded 'release' $pwshExe @('scripts/test.ps1', '-Preset', 'ucrt64-release') }
    }
} finally {
    if ($displayLease) {
        try { $displayLease.AssertValid() } finally { $displayLease.Dispose() }
    }
    if (Test-Path -LiteralPath 'build/ucrt64-release/Testing/Temporary/LastTest.log') {
        Copy-Item -LiteralPath 'build/ucrt64-release/Testing/Temporary/LastTest.log' -Destination (Join-Path $evidenceRoot 'ctest-raw.log')
    }
    Pop-Location
}
Write-Host "証拠: $evidenceRoot"
