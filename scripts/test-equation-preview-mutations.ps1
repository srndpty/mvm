# P3-4 の検査が空振りでないことを、build 配下の複製へ入れた変異で確かめる。製品 source は変えない。
# 変異した file は compile_commands.json の同じ compile 命令で object にし、対象の試験は ninja の
# 同じ link 命令で (元の object・library の member の代わりに変異の object を使って) 作り直す。
# 終了コード 1 と対象の検査の失敗メッセージを照合する。クラッシュ・timeout・compile error は
# 検出に数えない (runner の失敗として止める)。
[CmdletBinding()]
param([string]$Preset = 'ucrt64-release')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build/$Preset"
$ninja = 'C:/msys64/ucrt64/bin/ninja.exe'
$env:PATH = "C:/msys64/ucrt64/bin;$env:PATH"
$outputDir = Join-Path $buildDir ("equation-preview-mutations-" + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
[void](New-Item -ItemType Directory -Path $outputDir)

$compileCommands = Get-Content (Join-Path $buildDir 'compile_commands.json') -Raw | ConvertFrom-Json

function Get-CompileEntry([string]$File, [string]$TargetDir) {
    $full = (Join-Path $repoRoot $File).Replace('\', '/')
    $entry = $compileCommands | Where-Object {
        $_.file.Replace('\', '/') -eq $full -and $_.output.Replace('\', '/') -like "*/$TargetDir/*"
    } | Select-Object -First 1
    if (-not $entry) { throw "compile 命令がありません: $File ($TargetDir)" }
    return $entry
}

function Get-LinkCommand([string]$Executable) {
    $command = & $ninja -C $buildDir -t commands "bin/$Executable.exe" | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0 -or -not $command) { throw "link 命令を取れません: $Executable" }
    return $command
}

$pure = @{ Executable = 'mvm_test_equation_preview'; TargetDir = 'mvm_equation_sequence_compile.dir'
           File = 'src/app/equation_sequence_preview.cpp'; Arguments = @(); Timeout = 60000 }
$controller = @{ Executable = 'mvm_test_equation_preview_controller'
                 TargetDir = 'mvm_test_equation_preview_controller.dir'
                 File = 'apps/mvm/mvm_controller.cpp'; Arguments = @(); Timeout = 900000 }
$cache = @{ Executable = 'mvm_test_equation_preview_controller'; TargetDir = 'mvm_math_raster_cache.dir'
            File = 'apps/mvm/math_raster_cache.cpp'; Arguments = @(); Timeout = 900000 }

$mutations = @(
    # 変形の frame i に (i+1)/N の frame を見せる。
    @{ Name = 'transition_next_frame'; Target = $pure
       From = 'return {EquationPreviewShownKind::Transition, lookup.transition, lookup.frame};'
       To = 'return {EquationPreviewShownKind::Transition, lookup.transition, lookup.frame + 1};'
       Expected = '変形 frame 0 は frame 0 の被覆を source の位置に' },
    # base か accent の片方だけで action を見せる。
    @{ Name = 'half_action_shown'; Target = $pure
       From = 'return baseReady && accentReady;'; To = 'return baseReady || accentReady;'
       Expected = 'accent が無い action は静止' },
    # 層が届いた action を区間の先頭からやり直す。
    @{ Name = 'action_restart_on_join'; Target = $pure
       From = 'return {EquationPreviewShownKind::Action, lookup.action, lookup.frame};'
       To = 'return {EquationPreviewShownKind::Action, lookup.action, 0};'
       Expected = '読めた中央の frame へそのまま入り' },
    # 新しい key の artifact が揃うまで、前の key の animation (last-good) を見せ続ける。
    @{ Name = 'stale_animation_reused'; Target = $controller
       From = "    if (renderSpec &&`n        mathRasters_->requestEquationSequence(*renderSpec).state == MathRasterCache::State::Ready) {"
       To = "    if (!(renderSpec && mathRasters_->requestEquationSequence(*renderSpec).state ==`n                           MathRasterCache::State::Ready))`n        if (const auto previous = equationPreviewAnimations_.constFind(clipId);`n            previous != equationPreviewAnimations_.constEnd())`n            return previous->animation;`n    if (renderSpec &&`n        mathRasters_->requestEquationSequence(*renderSpec).state == MathRasterCache::State::Ready) {"
       Expected = '新しい artifact が揃うまで前の key の変形を使わず' },
    # 変形の色を provenance でなく今の Project (状態 0 の色) から作り直す。
    @{ Name = 'color_recomputed'; Target = $controller
       From = 'out.colors.push_back(frame.colorArgb); // provenance の色'
       To = 'out.colors.push_back(renderSpec->states[0].foregroundArgb);'
       Expected = '提示: 参照と全画素一致: T0 中央' },
    # 外側の不透明度を sequence の layer に 2 回掛ける。
    @{ Name = 'clip_effects_twice'; Target = $controller
       From = "                attachClipMotion(layer, effects, clip, project_);`n                composition->layers.push_back(std::move(layer));`n                continue;`n            }`n            if (stillMapping.kind == project::TimelineClipKind::Image ||"
       To = "                layer.opacity *= static_cast<float>(effects.opacityPercent / 100.0);`n                attachClipMotion(layer, effects, clip, project_);`n                composition->layers.push_back(std::move(layer));`n                continue;`n            }`n            if (stillMapping.kind == project::TimelineClipKind::Image ||"
       Expected = 'effects: 位置・拡大・回転・不透明度を layer に 1 回だけ掛ける' },
    # 編集で key が変わっても前の key の層を memory に残す。
    @{ Name = 'old_layers_retained'; Target = $cache
       From = 'if (!keys.contains(residentOwner(key)))'
       To = "if (!keys.contains(key) && !key.contains(QLatin1Char('/')))"
       Expected = '前の key の層は memory から外す' },
    # 層を読むときに provenance を照合しない。
    @{ Name = 'provenance_unchecked'; Target = $cache
       From = 'if (!equationSequenceProvenanceCurrent(directory, sequenceKey, provenance)) {'
       To = 'if (false) {'
       Expected = '壊れ: provenance を読むと disk を Failed' },
    # action の 2 層を束でなく 1 層ずつ予約する (片方だけ読む)。
    @{ Name = 'bundle_not_atomic'; Target = $cache
       From = "    auto reserved = residency_->tryReserve(bytes);`n    while (!reserved && current"
       To = "    missing.resize(1);`n    bytes = missing.front().bytes;`n    auto reserved = residency_->tryReserve(bytes);`n    while (!reserved && current"
       Expected = '予算: 1 層だけを読んで上限を使わない' }
)

$killed = 0
foreach ($mutation in $mutations) {
    $target = $mutation.Target
    $sourcePath = Join-Path $repoRoot $target.File
    $original = [IO.File]::ReadAllText($sourcePath).Replace("`r`n", "`n")
    if (-not $original.Contains($mutation.From)) { throw "変異箇所がありません: $($mutation.Name)" }
    $mutant = $original.Replace($mutation.From, $mutation.To)
    $source = Join-Path $outputDir "$($mutation.Name).cpp"
    $object = Join-Path $outputDir "$($mutation.Name).obj"
    $binary = Join-Path $outputDir "$($mutation.Name).exe"
    [IO.File]::WriteAllText($source, $mutant, [Text.UTF8Encoding]::new($false))

    $entry = Get-CompileEntry $target.File $target.TargetDir
    $sourceDir = Split-Path -Parent $sourcePath
    $compile = $entry.command -replace '(^\S+g\+\+\.exe)', "`$1 -iquote `"$sourceDir`""
    $compile = $compile -replace '\s-o\s+\S+', " -o `"$object`""
    $compile = $compile -replace '\s-c\s+\S+', " -c `"$source`""
    Push-Location $entry.directory
    try {
        & cmd.exe /c $compile 2>&1 | Out-File -Encoding utf8 (Join-Path $outputDir "$($mutation.Name).compile.log")
        if ($LASTEXITCODE -ne 0) { throw "変異のコンパイルが失敗しました: $($mutation.Name)" }
        $link = Get-LinkCommand $target.Executable
        $originalObject = $entry.output.Replace('\', '/').Replace(($buildDir.Replace('\', '/') + '/'), '')
        if ($link.Contains($originalObject)) {
            $link = $link.Replace($originalObject, "`"$object`"")
        } else {
            # library の member: 変異の object を先に置く (同じ symbol は archive から取らない)。
            $link = $link -replace '(g\+\+\.exe\s+(?:-\S+\s+)*)', "`$1`"$object`" "
        }
        $link = $link -replace '\s-o\s+\S+', " -o `"$binary`""
        $link = $link -replace '\s-Wl,--out-implib,\S+', ''
        & cmd.exe /c $link 2>&1 | Out-File -Encoding utf8 (Join-Path $outputDir "$($mutation.Name).link.log")
        if ($LASTEXITCODE -ne 0) { throw "変異の link が失敗しました: $($mutation.Name)" }
    } finally {
        Pop-Location
    }

    $startInfo = [Diagnostics.ProcessStartInfo]::new($binary)
    foreach ($argument in $target.Arguments) { $startInfo.ArgumentList.Add($argument) }
    $startInfo.WorkingDirectory = $outputDir
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($startInfo)
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($target.Timeout)) { $process.Kill($true); throw "変異が timeout しました: $($mutation.Name)" }
    $log = $stdoutTask.GetAwaiter().GetResult() + $stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $outputDir "$($mutation.Name).log"), $log, [Text.UTF8Encoding]::new($false))
    if ($process.ExitCode -ne 1 -or -not $log.Contains($mutation.Expected)) {
        throw "変異を対象の検査で検出できません: $($mutation.Name) / 終了 $($process.ExitCode)"
    }
    ++$killed
    Write-Host "変異検出: $($mutation.Name)"
}
Write-Host "$killed / $($mutations.Count) 変異検出。証拠: $outputDir"
