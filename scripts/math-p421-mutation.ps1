<#
.SYNOPSIS
    staging 中の長時間 mutex 保持を復元し、応答性の負例を検査する。
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
if (Test-Path -LiteralPath $EvidenceDirectory) { throw '証拠は既存です' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$file = Join-Path $repoRoot 'src/media/graph/graph_render.cpp'
$original = [IO.File]::ReadAllText($file)
$hash = (Get-FileHash -LiteralPath $file).Hash
$changes = @(
    @{ before = '    std::filesystem::create_directories(cache, ec);'; after = '    std::lock_guard stagingLock(mutex_); std::filesystem::create_directories(cache, ec);' },
    @{ before = 'if (auto error = checkAuthority())'; after = 'if (auto error = guard())' },
    @{ before = '        std::lock_guard lock(mutex_);' + "`n" + '        if (auto error = guard())' + "`n" + '            return *error;' + "`n" + '        // この非置換 rename'; after = '        if (auto error = guard())' + "`n" + '            return *error;' + "`n" + '        // この非置換 rename' }
)
# source の改行を保ち、退避と復元の hash を必ず照合する。
$newline = if ($original.Contains("`r`n")) { "`r`n" } else { "`n" }
$mutated = $original
foreach ($change in $changes) {
    $before = $change.before.Replace("`n", $newline)
    $after = $change.after.Replace("`n", $newline)
    if (-not $mutated.Contains($before)) { throw '変異箇所がありません' }
    $mutated = $mutated.Replace($before, $after)
}
[IO.File]::WriteAllText((Join-Path $EvidenceDirectory 'original.cpp'), $original)
[IO.File]::WriteAllText((Join-Path $EvidenceDirectory 'mutated.cpp'), $mutated)
$pwshExe = (Get-Process -Id $PID).Path
try {
    [IO.File]::WriteAllText($file, $mutated)
    & $pwshExe scripts/build.ps1 -Target mvm_test_graph_render -ReuseConfigure *> (Join-Path $EvidenceDirectory 'build.log')
    $buildCode = $LASTEXITCODE
    if ($buildCode -ne 0) { throw '変異がビルドできません' }
    & build/ucrt64-release/bin/mvm_test_graph_render.exe (Join-Path $EvidenceDirectory 'artifacts') responsiveness *> (Join-Path $EvidenceDirectory 'test.log')
    $testCode = $LASTEXITCODE
    $detected = $testCode -eq 1 -and (Select-String -LiteralPath (Join-Path $EvidenceDirectory 'test.log') -Pattern 'staging を解放せずに authority 無効化が完了する' -Quiet)
    @{ name = 'long-held-publication-mutex'; build_exit = $buildCode; test_exit = $testCode; killed = $detected; original_sha256 = $hash } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'mutation.json') -Encoding utf8NoBOM
    if (-not $detected) { throw '応答性の負例を検出できません' }
} finally {
    [IO.File]::WriteAllText($file, $original)
    if ((Get-FileHash -LiteralPath $file).Hash -ne $hash) { throw '復元 hash が一致しません' }
    & $pwshExe scripts/build.ps1 -Target mvm_test_graph_render -ReuseConfigure *> (Join-Path $EvidenceDirectory 'restored-build.log')
    if ($LASTEXITCODE -ne 0) { throw '復元後のビルドが失敗しました' }
}
Write-Host '長時間 mutex の変異を応答性検査で検出し、source を復元しました'
