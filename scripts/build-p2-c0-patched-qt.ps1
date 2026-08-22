[CmdletBinding()]
param(
    [string]$SourceDirectory=(Join-Path (Split-Path -Parent $PSScriptRoot) 'third_party\qtbase-v6.11.1'),
    [string]$BuildDirectory=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtbase-c0'),
    [string]$ProvenanceJson=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtbase-c0\mvm-c0-provenance.json'),
    [ValidateRange(1,16)][int]$Jobs=4
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$repo=Split-Path -Parent $PSScriptRoot
$patch=Join-Path $repo 'qt-patches\qtbase-6.11.1\0001-mvm-native-present-hook.patch'
$abi=Join-Path $repo 'src\app\preview\native_present_hook_abi.h'
foreach($path in @($SourceDirectory,$patch,$abi)){
    if(-not(Test-Path -LiteralPath $path)){throw "C0 Qt build必須pathがありません: $path"}
}
$SourceDirectory=(Resolve-Path -LiteralPath $SourceDirectory).Path
$upstream=(& git -C $SourceDirectory rev-parse HEAD).Trim()
if($LASTEXITCODE-ne0-or$upstream-ne'59c81a3c2247b821b9b84b4eb8d939b77e07e276'){
    throw "Qt upstream commitがv6.11.1固定値と一致しません: $upstream"
}
& git -C $SourceDirectory apply --reverse --check $patch
if($LASTEXITCODE-ne0){throw 'Qt sourceが固定patchと完全一致しません'}
$env:PATH="C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:PATH"
$cmake='C:\msys64\ucrt64\bin\cmake.exe'
$ninja='C:\msys64\ucrt64\bin\ninja.exe'
if(-not(Test-Path -LiteralPath $BuildDirectory)){New-Item -ItemType Directory -Path $BuildDirectory|Out-Null}
$BuildDirectory=(Resolve-Path -LiteralPath $BuildDirectory).Path
$includeFlag='-I'+($repo-replace'\\','/')+'/src'
& $cmake -S $SourceDirectory -B $BuildDirectory -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    -DBUILD_SHARED_LIBS=ON `
    -DQT_BUILD_TESTS=OFF `
    -DQT_BUILD_EXAMPLES=OFF `
    -DQT_BUILD_BENCHMARKS=OFF `
    "-DCMAKE_CXX_FLAGS=$includeFlag"
if($LASTEXITCODE-ne0){throw 'patched QtBase configureに失敗しました'}
& $cmake --build $BuildDirectory --target Gui QWindowsIntegrationPlugin --parallel $Jobs
if($LASTEXITCODE-ne0){throw 'patched QtGui buildに失敗しました'}
$qtGui=Join-Path $BuildDirectory 'bin\Qt6Gui.dll'
$qtCore=Join-Path $BuildDirectory 'bin\Qt6Core.dll'
$qwindows=Join-Path $BuildDirectory 'plugins\platforms\qwindows.dll'
foreach($path in @($qtGui,$qtCore,$qwindows)){
    if(-not(Test-Path -LiteralPath $path)){throw "patched Qt runtimeがありません: $path"}
}
$provenance=[ordered]@{
    schema='mvm-p2-c0-patched-qt-provenance-1'
    qt_upstream_tag='v6.11.1'
    qt_upstream_commit=$upstream
    patch='qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch'
    patch_sha256=Hash $patch
    abi_header='src/app/preview/native_present_hook_abi.h'
    abi_header_sha256=Hash $abi
    qt_gui_dll=$qtGui
    qt_gui_dll_sha256=Hash $qtGui
    qt_core_dll=$qtCore
    qt_core_dll_sha256=Hash $qtCore
    qwindows_dll=$qwindows
    qwindows_dll_sha256=Hash $qwindows
    build_type='RelWithDebInfo'
    hook_default_enabled=$false
}
$provenance|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $ProvenanceJson -Encoding utf8
Write-Host "F3-C0 patched QtGui build: PASS ($qtGui)"
