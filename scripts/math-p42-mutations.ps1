<#
.SYNOPSIS
    P4-2 の17変異を正常なビルドと明示的な検証失敗で検出する。
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$EvidenceDirectory,
      [string]$Python = "$env:APPDATA/uv/tools/manim/Scripts/python.exe")
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
if (Test-Path -LiteralPath $EvidenceDirectory) { throw '変異の証拠 directory は既存です' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$evidenceRoot = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$exe = Join-Path $repoRoot 'build/ucrt64-release/bin/mvm_test_graph_render.exe'
$pwshExe = (Get-Process -Id $PID).Path
$graphFile = Join-Path $repoRoot 'src/media/graph/graph_render.cpp'
$compileFile = Join-Path $repoRoot 'src/app/graph_render_compile.cpp'
$backendFile = Join-Path $repoRoot 'src/media/manim/graph_backend.py'
$cases = @(
    @{ name = 'backend-resamples'; file = $backendFile; mode = 'geometry'; changes = @(@('.set_points_as_corners(mapped)', '.set_points_as_corners(mapped[::2])')) },
    @{ name = 'segment-boundaries-ignored'; file = $backendFile; mode = 'geometry'; changes = @(@('for frame in frames:', @'
for frame in frames:
            owned = [path for path in frame["paths"] if path["function"] == 0]
            if len(owned) > 1:
                frame["paths"] = [{**owned[0], "points": sum([path["points"] for path in owned], [])}]
'@)) },
    @{ name = 'function-order-reversed'; file = $backendFile; mode = 'order'; changes = @(@('enumerate(frame["paths"])', 'enumerate(list(reversed(frame["paths"])))')) },
    @{ name = 'alpha-forced-255'; file = $backendFile; mode = 'alpha'; changes = @(@('(argb >> 24)', '255')) },
    @{ name = 'premultiplied-as-straight'; file = $backendFile; mode = 'alpha'; changes = @(@('Image.fromarray(pixels).save(name)', @'
pixels[:, :, :3] = (pixels[:, :, :3].astype(np.uint32) * pixels[:, :, 3:4] // 255).astype(np.uint8)
            Image.fromarray(pixels).save(name)
'@)) },
    @{ name = 'stroke-scaling-disabled'; file = $backendFile; mode = 'stroke'; changes = @(@('stroke_width=path["width"] / camera', 'stroke_width=3.0 / camera')) },
    @{ name = 'grid-tied-to-axes'; file = $compileFile; changes = @(@('if (data.axes.showGrid)', 'if (data.axes.showGrid && data.axes.showAxes)')) },
    @{ name = 'label-in-plot'; file = $backendFile; mode = 'labels'; changes = @(@('glyph.move_to([left + bw / 2 - width / 2, height / 2 - top - bh / 2, 0])', 'glyph.move_to([0, 0, 0])')) },
    @{ name = 'draw-i-plus-one'; file = $graphFile; changes = @(@('reveal(curve.geometry, frame, spec.drawFrames)', 'reveal(curve.geometry, frame + 1, spec.drawFrames)')) },
    @{ name = 'draw-backend-time-authority'; file = $backendFile; mode = 'draw'; changes = @(@('for frame in frames:', @'
for original_frame in frames:
            frame = {**original_frame, "paths": frames[0]["paths"]}
'@)) },
    @{ name = 'ownership-in-static-key'; file = $graphFile; changes = @(@('field(out, c.ast);', 'field(out, c.ast); field(out, c.geometry.functionId);')) },
    @{ name = 'draw-n-in-static-key'; file = $graphFile; changes = @(@('field(out, canonicalSpec(spec));', 'field(out, canonicalSpec(spec)); field(out, std::to_string(spec.drawFrames));')) },
    @{ name = 'provenance-before-validation'; file = $graphFile; changes = @(@('auto checked = inspectFrames(request.job, request.spec, request.toolchain, &stop);', 'std::ofstream(request.job / "manifest.txt") << "premature"; auto checked = inspectFrames(request.job, request.spec, request.toolchain, &stop);')) },
    @{ name = 'missing-draw-frame-ready'; file = $graphFile; changes = @(@('for (std::int64_t i = -1; i < spec.drawFrames; ++i) {', 'for (std::int64_t i = -1; i < 0; ++i) {')) },
    @{ name = 'sha-validation-skipped'; file = $graphFile; changes = @(@('digest({reinterpret_cast<const char*>(rgba.data()), rgba.size()})', 'std::string(64, ''0'')'), @('const auto encodedHash = digest(encoded);', 'const auto encodedHash = std::string(64, ''0'');')) },
    @{ name = 'stale-publishes-ready'; file = $graphFile; changes = @(@('if (generation != generation_)', 'if (false)'), @('generationCancel_->store(true);', 'generationCancel_->store(false);')) },
    @{ name = 'cancelled-publishes-ready'; file = $graphFile; changes = @(@('return cancel && cancel->load();', 'return false;')) }
)
$records = [System.Collections.Generic.List[object]]::new()
foreach ($case in $cases) {
    $caseRoot = Join-Path $evidenceRoot $case.name
    $null = New-Item -ItemType Directory -Path $caseRoot
    $original = [IO.File]::ReadAllText($case.file)
    $originalHash = (Get-FileHash -LiteralPath $case.file -Algorithm SHA256).Hash
    $mutated = $original
    $changes = $case.changes
    if ($changes[0] -is [string]) { $changes = ,$changes }
    foreach ($change in $changes) {
        if (-not $mutated.Contains($change[0])) { throw "変異箇所がありません: $($case.name) / $($change[0])" }
        $mutated = $mutated.Replace($change[0], $change[1])
    }
    [IO.File]::WriteAllText((Join-Path $caseRoot 'original.txt'), $original)
    [IO.File]::WriteAllText((Join-Path $caseRoot 'mutated.txt'), $mutated)
    $isBackend = $case.file -eq $backendFile
    $actualBackend = $backendFile
    $buildCode = 0
    try {
        if ($isBackend) {
            $actualBackend = Join-Path $caseRoot 'backend.py'
            [IO.File]::WriteAllText($actualBackend, $mutated)
        } else {
            [IO.File]::WriteAllText($case.file, $mutated)
            & $pwshExe (Join-Path $PSScriptRoot 'build.ps1') -Target mvm_test_graph_render -ReuseConfigure *> (Join-Path $caseRoot 'build.log')
            $buildCode = $LASTEXITCODE
        }
        if ($buildCode -ne 0) { throw "変異はビルド失敗です。kill に数えません: $($case.name)" }
        $arguments = @((Join-Path $caseRoot 'artifacts'))
        if ($isBackend) { $arguments += @($Python, $actualBackend, $case.mode) }
        & $exe @arguments *> (Join-Path $caseRoot 'test.log')
        $code = $LASTEXITCODE
        $records.Add([ordered]@{ name = $case.name; build_exit = $buildCode; test_exit = $code;
            killed = ($code -eq 1); original_sha256 = $originalHash; evidence = $caseRoot })
        $records | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'mutations.json') -Encoding utf8NoBOM
        Write-Host "$($case.name): build=$buildCode test=$code"
        if ($code -ne 1) { throw '変異は assertion による終了コード1で検出されませんでした' }
    } finally {
        if (-not $isBackend) {
            [IO.File]::WriteAllText($case.file, $original)
            if ((Get-FileHash -LiteralPath $case.file -Algorithm SHA256).Hash -ne $originalHash) { throw 'source を復元できません' }
        }
    }
}
& $pwshExe (Join-Path $PSScriptRoot 'build.ps1') -Target mvm_test_graph_render -ReuseConfigure *> (Join-Path $evidenceRoot 'restored-build.log')
if ($LASTEXITCODE -ne 0) { throw '復元後のビルドに失敗しました' }
Write-Host "変異 $($records.Count)/17 件を検出しました"
