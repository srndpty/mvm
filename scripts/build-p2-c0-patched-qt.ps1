[CmdletBinding()]
param(
    [string]$SourceDirectory=(Join-Path (Split-Path -Parent $PSScriptRoot) 'third_party\qtbase-v6.11.1'),
    [string]$BuildDirectory=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtbase-c0'),
    [string]$DeclarativeSourceDirectory=(Join-Path (Split-Path -Parent $PSScriptRoot) 'third_party\qtdeclarative-v6.11.1'),
    [string]$DeclarativeBuildDirectory=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtdeclarative-t2-system-base'),
    [string]$QuickRuntimeDirectory=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtquick-t2-runtime'),
    [string]$ProvenanceJson=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qtbase-c0\mvm-c0-provenance.json'),
    [ValidateRange(1,16)][int]$Jobs=4
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$repo=Split-Path -Parent $PSScriptRoot
$patch=Join-Path $repo 'qt-patches\qtbase-6.11.1\0001-mvm-native-present-hook.patch'
$t2Patch=Join-Path $repo 'qt-patches\qtbase-6.11.1\0002-mvm-dirty-propagation-hook.patch'
$quickPatch=Join-Path $repo 'qt-patches\qtdeclarative-6.11.1\0001-mvm-dirty-propagation-hook.patch'
$abi=Join-Path $repo 'src\app\preview\native_present_hook_abi.h'
foreach($path in @($SourceDirectory,$DeclarativeSourceDirectory,$patch,$t2Patch,$quickPatch,$abi)){
    if(-not(Test-Path -LiteralPath $path)){throw "C0 Qt build必須pathがありません: $path"}
}
$SourceDirectory=(Resolve-Path -LiteralPath $SourceDirectory).Path
$upstream=(& git -C $SourceDirectory rev-parse HEAD).Trim()
if($LASTEXITCODE-ne0-or$upstream-ne'59c81a3c2247b821b9b84b4eb8d939b77e07e276'){
    throw "Qt upstream commitがv6.11.1固定値と一致しません: $upstream"
}
& git -C $SourceDirectory apply --reverse --check $t2Patch
if($LASTEXITCODE-ne0){throw 'QtBase sourceがC0+T2合成patchと完全一致しません'}
& git -C $SourceDirectory diff --check
if($LASTEXITCODE-ne0){throw 'QtBase sourceのpatch差分が不正です'}
$declarativeUpstream=(& git -C $DeclarativeSourceDirectory rev-parse HEAD).Trim()
if($LASTEXITCODE-ne0-or$declarativeUpstream-ne'a02bed441965ee1f18f856352c7d5ee5ba35d795'){
    throw "QtDeclarative upstream commitがv6.11.1固定値と一致しません: $declarativeUpstream"
}
& git -C $DeclarativeSourceDirectory apply --reverse --check $quickPatch
if($LASTEXITCODE-ne0){throw 'QtDeclarative sourceがT2固定patchと完全一致しません'}
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
if(-not(Test-Path -LiteralPath $DeclarativeBuildDirectory)){New-Item -ItemType Directory -Path $DeclarativeBuildDirectory|Out-Null}
$DeclarativeBuildDirectory=(Resolve-Path -LiteralPath $DeclarativeBuildDirectory).Path
& $cmake -S $DeclarativeSourceDirectory -B $DeclarativeBuildDirectory -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    -DBUILD_SHARED_LIBS=ON -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF `
    -DQT_BUILD_BENCHMARKS=OFF "-DQt6_DIR=C:\msys64\ucrt64\lib\cmake\Qt6" `
    "-DQt6ShaderTools_DIR=C:\msys64\ucrt64\lib\cmake\Qt6ShaderTools" `
    "-DQt6ShaderToolsTools_DIR=C:\msys64\ucrt64\lib\cmake\Qt6ShaderToolsTools" `
    "-DCMAKE_CXX_FLAGS=$includeFlag"
if($LASTEXITCODE-ne0){throw 'patched QtDeclarative configureに失敗しました'}
& $cmake --build $DeclarativeBuildDirectory --target Quick --parallel $Jobs
if($LASTEXITCODE-ne0){throw 'patched QtQuick buildに失敗しました'}
$qtGui=Join-Path $BuildDirectory 'bin\Qt6Gui.dll'
$qtCore=Join-Path $BuildDirectory 'bin\Qt6Core.dll'
$qwindows=Join-Path $BuildDirectory 'plugins\platforms\qwindows.dll'
$builtQtQuick=Join-Path $DeclarativeBuildDirectory 'bin\Qt6Quick.dll'
if(-not(Test-Path -LiteralPath $builtQtQuick)){throw "patched QtQuick build成果物がありません: $builtQtQuick"}
if(-not(Test-Path -LiteralPath $QuickRuntimeDirectory)){New-Item -ItemType Directory -Path $QuickRuntimeDirectory|Out-Null}
$qtQuick=Join-Path $QuickRuntimeDirectory 'Qt6Quick.dll'
Copy-Item -LiteralPath $builtQtQuick -Destination $qtQuick -Force
foreach($path in @($qtGui,$qtCore,$qwindows,$qtQuick)){
    if(-not(Test-Path -LiteralPath $path)){throw "patched Qt runtimeがありません: $path"}
}
$provenance=[ordered]@{
    schema='mvm-p2-c0-patched-qt-provenance-1'
    qt_upstream_tag='v6.11.1'
    qt_upstream_commit=$upstream
    patch='qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch'
    patch_sha256=Hash $patch
    t2_qtbase_patch_sha256=Hash $t2Patch
    qtdeclarative_upstream_commit=$declarativeUpstream
    qtdeclarative_patch_sha256=Hash $quickPatch
    abi_header='src/app/preview/native_present_hook_abi.h'
    abi_header_sha256=Hash $abi
    qt_gui_dll=$qtGui
    qt_gui_dll_sha256=Hash $qtGui
    qt_core_dll=$qtCore
    qt_core_dll_sha256=Hash $qtCore
    qwindows_dll=$qwindows
    qwindows_dll_sha256=Hash $qwindows
    qt_quick_dll=$qtQuick
    qt_quick_dll_sha256=Hash $qtQuick
    build_type='RelWithDebInfo'
    hook_default_enabled=$false
}
$provenance|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $ProvenanceJson -Encoding utf8
Write-Host "F3-C0 patched QtGui build: PASS ($qtGui)"
