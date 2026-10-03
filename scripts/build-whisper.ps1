[CmdletBinding()]
param([string]$Ucrt64 = 'C:\msys64\ucrt64', [int]$Jobs = 4)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'lib/cmake-toolchain.ps1')
Set-MvmUcrt64Environment -Ucrt64 $Ucrt64
$repoRoot = Split-Path -Parent $PSScriptRoot
$revision = '2ca53bb45e38748d07b310eeb36245a7157ac882'
$sourceDir = Join-Path $repoRoot "build/whisper-source-$revision"
$binaryDir = Join-Path $repoRoot 'build/whisper-build'
$installDir = Join-Path $repoRoot 'build/whisper-install'
$gitCommand = Get-Command git -ErrorAction SilentlyContinue
$gitExecutable = if ($gitCommand) { $gitCommand.Source } elseif (Test-Path 'C:\Program Files\Git\cmd\git.exe') { 'C:\Program Files\Git\cmd\git.exe' } else { throw 'gitが見つかりません' }
if (-not (Test-Path $sourceDir)) {
    & $gitExecutable init $sourceDir
    if ($LASTEXITCODE) { throw 'Whisperの作業ディレクトリを作成できません' }
    & $gitExecutable -C $sourceDir fetch --depth 1 https://github.com/ggml-org/whisper.cpp.git $revision
    if ($LASTEXITCODE) { throw '固定リビジョンのWhisperを取得できません' }
    & $gitExecutable -C $sourceDir checkout --detach FETCH_HEAD
    if ($LASTEXITCODE) { throw 'Whisperの固定リビジョンを展開できません' }
}
$actualRevision = (& $gitExecutable -C $sourceDir rev-parse HEAD).Trim()
if ($LASTEXITCODE -or $actualRevision -ne $revision) { throw 'Whisperのソースリビジョンが一致しません' }
& $gitExecutable -C $sourceDir config core.abbrev 9
if ($LASTEXITCODE) { throw 'Whisperのリビジョン表記を設定できません' }
$sourceStatus = @(& $gitExecutable -C $sourceDir status --porcelain -- . ':!bindings/javascript/package.json')
if ($sourceStatus.Count) { throw 'Whisperのソースに未確定の変更があります' }
$cmakeExecutable = Join-Path $Ucrt64 'bin/cmake.exe'
& $cmakeExecutable -S $sourceDir -B $binaryDir -G Ninja "-DCMAKE_C_COMPILER=$Ucrt64/bin/gcc.exe" "-DCMAKE_CXX_COMPILER=$Ucrt64/bin/g++.exe" "-DCMAKE_MAKE_PROGRAM=$Ucrt64/bin/ninja.exe" "-DCMAKE_PREFIX_PATH=$Ucrt64" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$installDir" -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=OFF -DGGML_VULKAN=ON -DGGML_BLAS=OFF -DGGML_OPENCL=OFF -DBUILD_SHARED_LIBS=ON -DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=OFF -DGGML_NATIVE=OFF
if ($LASTEXITCODE) { throw 'Whisperのconfigureに失敗しました' }
& $cmakeExecutable --build $binaryDir --parallel $Jobs
if ($LASTEXITCODE) { throw 'Whisperのビルドに失敗しました' }
& $cmakeExecutable --install $binaryDir
if ($LASTEXITCODE) { throw 'Whisperの導入に失敗しました' }
Write-Host "固定リビジョンのCPU／Vulkan版を構築しました。次に pwsh scripts/build.ps1 -WhisperRoot `"$installDir`" を実行してください。"
