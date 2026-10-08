<#
.SYNOPSIS
    P4-1 の決定的な変異を一件ずつ検査し、source を byte 単位で復元する。
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$pwshExe = (Get-Process -Id $PID).Path
$ctestExe = 'C:\msys64\ucrt64\bin\ctest.exe'
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$numeric = 'src/media/graph/graph_numeric.cpp'
$domain = 'src/project/graph_clip.cpp'
$mapping = 'src/project/graph_edit.cpp'
$json = 'src/project/project_json.cpp'
$controller = 'apps/mvm/mvm_controller.cpp'
$cases = @(
    @{ name = 'unary-precedence'; file = $numeric; old = 'return node(Operation::Negative, unary(d + 1));'; new = 'auto a = node(Operation::Negative, atom(d + 1)); if (take(''^'')) a = node(Operation::Power, a, unary(d + 1)); return a;' },
    @{ name = 'left-power'; file = $numeric; old = @'
        if (result_.status == CompileStatus::Success && take('^')) {
            if (!descend(d))
                return absent;
            a = node(Operation::Power, a, unary(d + 1));
        }
'@; new = @'
        while (result_.status == CompileStatus::Success && take('^')) {
            if (!descend(d))
                return absent;
            a = node(Operation::Power, a, atom(d + 1));
        }
'@ },
    @{ name = 'parenthesis-recursion'; file = $numeric; old = @'
        if (take('(')) {
            if (!descend(d))
                return absent;
'@; new = '        if (take(''('')) {' },
    @{ name = 'function-recursion'; file = $numeric; old = @'
        if (!descend(d))
            return absent;
        auto a = sum(d + 1);
'@; new = '        auto a = sum(d + 1);' },
    @{ name = 'node-budget'; file = $numeric; old = 'nodes.size() >= nodeBudget_'; new = 'false' },
    @{ name = 'unknown-identifier'; file = $numeric; old = @'
            result_.status = CompileStatus::UnsupportedExpression;
            return absent;
'@; new = '            return node(Operation::X);' },
    @{ name = 'midpoint-connected'; file = $numeric; old = @'
                ++out.diagnostics.midpointFailures;
                edge = false;
'@; new = @'
                ++out.diagnostics.midpointFailures;
                edge = true;
'@ },
    @{ name = 'jump-connected'; file = $numeric; old = @'
                    ++out.diagnostics.jumpDiscardedEdges;
                    edge = false;
'@; new = @'
                    ++out.diagnostics.jumpDiscardedEdges;
                    edge = true;
'@ },
    @{ name = 'clipping-bypassed'; file = $numeric; old = 'if (!clipEdge(a, b, r))'; new = 'if (false)' },
    @{ name = 'draw-restarted'; file = $mapping; old = 'clipSourceFrameAt(clip, output.num, output.den, local)'; new = 'clipSourceFrameAt(clip, output.num, output.den, local)'; pattern = '^graph_domain$'; target = 'mvm_test_graph_domain'; extraOld = 'frame.progressNumerator = source.frame;'; extraNew = 'frame.progressNumerator = source.frame - clip.sourceInFrame;' },
    @{ name = 'edit-id-regenerated'; file = $domain; old = '*it = f;'; new = '*it = f; it->id.value = "mutant";'; pattern = '^graph_domain$'; target = 'mvm_test_graph_domain' },
    @{ name = 'split-id-reused'; file = 'src/project/timeline_edit.cpp'; old = '!remapGraphIds(right.graph, newId, result.error)'; new = 'false'; pattern = '^graph_domain$'; target = 'mvm_test_graph_domain' },
    @{ name = 'invalid-expression-load-rejected'; file = $json; old = 'return parseString(f.expression);'; new = 'return parseString(f.expression) && f.expression != "sin(";'; pattern = '^graph_domain$'; target = 'mvm_test_graph_domain' },
    @{ name = 'unknown-json-accepted'; file = $json; old = @'
                if (it == fields.end())
                    return fail("数式 sequence に未知の field があります: " + key);
'@; new = @'
                if (it == fields.end()) {
                    if (!skipValue()) return false;
                    if (consumeIf(',')) continue;
                    break;
                }
'@; pattern = '^graph_domain$'; target = 'mvm_test_graph_domain' },
    @{ name = 'failed-edit-undo-changed'; file = $controller; old = @'
    const auto result = project::editGraph(candidate, clipId, edit);
    if (!result.success) {
'@; new = @'
    const auto result = project::editGraph(candidate, clipId, edit);
    if (!result.success) {
        pushUndoEntry(UndoEntry{});
'@; pattern = '^graph_controller_history$'; target = 'mvm_test_math_controller' }
)
$reports = [System.Collections.Generic.List[object]]::new()
try {
foreach ($case in $cases) {
    $path = Join-Path $repoRoot $case.file
    $beforeBytes = [IO.File]::ReadAllBytes($path)
    $beforeHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    $source = [Text.Encoding]::UTF8.GetString($beforeBytes).Replace("`r`n", "`n")
    if (-not $source.Contains($case.old)) { throw "変異対象がありません: $($case.name)" }
    $mutated = $source.Replace($case.old, $case.new)
    if ($case.ContainsKey('extraOld')) {
        if (-not $mutated.Contains($case.extraOld)) { throw '追加の変異対象がありません' }
        $mutated = $mutated.Replace($case.extraOld, $case.extraNew)
    }
    $target = if ($case.ContainsKey('target')) { $case.target } else { 'mvm_test_graph_numeric' }
    $pattern = if ($case.ContainsKey('pattern')) { $case.pattern } else { '^graph_numeric$' }
    $caseRoot = Join-Path $EvidenceDirectory $case.name
    $null = New-Item -ItemType Directory -Path $caseRoot
    $case | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $caseRoot 'mutation.json') -Encoding utf8NoBOM
    try {
        # AGENTS.md の controlled mutation 例外。元の byte 列を finally で復元して hash 照合する。
        [IO.File]::WriteAllText($path, $mutated, [Text.UTF8Encoding]::new($false))
        Copy-Item -LiteralPath $path -Destination (Join-Path $caseRoot 'mutated-source.cpp')
        & $pwshExe (Join-Path $PSScriptRoot 'build.ps1') -Target $target -ReuseConfigure 2>&1 |
            Tee-Object -FilePath (Join-Path $caseRoot 'build.log') | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { throw '変異は compile 失敗です。検出成功と数えません' }
        $buildDir = Join-Path $repoRoot 'build/ucrt64-release'
        $listing = (& $ctestExe --test-dir $buildDir -N -R $pattern) -join "`n"
        if ($LASTEXITCODE -ne 0 -or $listing -notmatch 'Total Tests: 1\s*$') { throw '変異の対象は正確に 1 テストでなければなりません' }
        & $ctestExe --test-dir $buildDir -R $pattern --output-on-failure --timeout 120 2>&1 |
            Tee-Object -FilePath (Join-Path $caseRoot 'test.log') | ForEach-Object { Write-Host $_ }
        $testExit = $LASTEXITCODE
        $raw = Join-Path $buildDir 'Testing/Temporary/LastTest.log'
        Copy-Item -LiteralPath $raw -Destination (Join-Path $caseRoot 'raw.log')
        $rawText = Get-Content -LiteralPath $raw -Raw
        if ($testExit -ne 8 -or $rawText -notmatch '(失敗:|FAIL:)' -or $rawText -match '(SegFault|Exception|Timeout)') {
            $reports.Add([ordered]@{ name = $case.name; result = 'NOT_DETECTED'; exit = $testExit; test_count = 1 })
            throw '変異を通常の assertion failure として検出できませんでした'
        }
        $reports.Add([ordered]@{ name = $case.name; source = $case.file; original_sha256 = $beforeHash;
            mutation_sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash;
            build_command = "pwsh scripts/build.ps1 -Target $target -ReuseConfigure";
            test_command = "ctest --test-dir build/ucrt64-release -R $pattern --output-on-failure --timeout 120";
            test_count = 1; exit = $testExit; result = 'DETECTED' })
    } finally {
        [IO.File]::WriteAllBytes($path, $beforeBytes)
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $beforeHash) { throw 'source の復元照合に失敗しました' }
        $reports | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'mutations.json') -Encoding utf8NoBOM
    }
}
} finally {
# 復元済み source の binary へ戻す。変異 binary を後続 gate へ持ち込まない。
foreach ($target in @('mvm_test_graph_numeric', 'mvm_test_graph_domain', 'mvm_test_math_controller')) {
    & $pwshExe (Join-Path $PSScriptRoot 'build.ps1') -Target $target -ReuseConfigure 2>&1 |
        Tee-Object -FilePath (Join-Path $EvidenceDirectory ('restored-' + $target + '.log')) | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw '復元 source の再ビルドに失敗しました' }
}
}
Write-Host "変異 $($reports.Count) 件を検出、source 復元済み"
exit 0
