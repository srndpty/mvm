[CmdletBinding()]
param(
    [string]$SourceDirectory=(Join-Path (Split-Path -Parent $PSScriptRoot) 'third_party\qtbase-v6.11.1'),
    [string]$DeclarativeSourceDirectory=(Join-Path (Split-Path -Parent $PSScriptRoot) 'third_party\qtdeclarative-v6.11.1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repo=Split-Path -Parent $PSScriptRoot
$patch=Join-Path $repo 'qt-patches\qtbase-6.11.1\0001-mvm-native-present-hook.patch'
$t2Patch=Join-Path $repo 'qt-patches\qtbase-6.11.1\0002-mvm-dirty-propagation-hook.patch'
$quickPatch=Join-Path $repo 'qt-patches\qtdeclarative-6.11.1\0001-mvm-dirty-propagation-hook.patch'
foreach($path in @($patch,$t2Patch,$quickPatch)){if(-not(Test-Path -LiteralPath $path)){throw "Qt診断patchがありません: $path"}}
if(-not(Test-Path -LiteralPath $SourceDirectory)){
    & git clone --branch v6.11.1 --depth 1 https://github.com/qt/qtbase.git $SourceDirectory
    if($LASTEXITCODE-ne0){throw 'QtBase v6.11.1 cloneに失敗しました'}
    & git -C $SourceDirectory apply $patch
    if($LASTEXITCODE-ne0){throw 'F3-C0 Qt patch適用に失敗しました'}
    & git -C $SourceDirectory apply $t2Patch
    if($LASTEXITCODE-ne0){throw 'F3-C3-A3-T2 QtBase patch適用に失敗しました'}
}
$upstream=(& git -C $SourceDirectory rev-parse HEAD).Trim()
if($LASTEXITCODE-ne0-or$upstream-ne'59c81a3c2247b821b9b84b4eb8d939b77e07e276'){
    throw "QtBase upstream commitが一致しません: $upstream"
}
# 0002は0001の追加領域を拡張するため、合成済みsourceでは0001単独のreverse
# checkは成立しない。最終patchで合成状態を検査し、両patchのhashをprovenanceへ固定する。
& git -C $SourceDirectory apply --reverse --check $t2Patch
if($LASTEXITCODE-ne0){throw 'QtBase sourceがC0+T2合成patchと一致しません'}
& git -C $SourceDirectory diff --check
if($LASTEXITCODE-ne0){throw 'QtBase sourceのpatch差分が不正です'}
if(-not(Test-Path -LiteralPath $DeclarativeSourceDirectory)){
    & git clone --branch v6.11.1 --depth 1 https://github.com/qt/qtdeclarative.git $DeclarativeSourceDirectory
    if($LASTEXITCODE-ne0){throw 'QtDeclarative v6.11.1 cloneに失敗しました'}
    & git -C $DeclarativeSourceDirectory apply $quickPatch
    if($LASTEXITCODE-ne0){throw 'F3-C3-A3-T2 QtQuick patch適用に失敗しました'}
}
$quickUpstream=(& git -C $DeclarativeSourceDirectory rev-parse HEAD).Trim()
if($LASTEXITCODE-ne0-or$quickUpstream-ne'a02bed441965ee1f18f856352c7d5ee5ba35d795'){
    throw "QtDeclarative upstream commitが一致しません: $quickUpstream"
}
& git -C $DeclarativeSourceDirectory apply --reverse --check $quickPatch
if($LASTEXITCODE-ne0){throw 'QtDeclarative sourceがT2固定patchと一致しません'}
Write-Host "F3-C3-A3-T2 Qt source: PASS (qtbase=$upstream qtdeclarative=$quickUpstream)"
