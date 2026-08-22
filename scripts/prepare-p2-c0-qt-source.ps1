[CmdletBinding()]
param(
    [string]$SourceDirectory=(Join-Path (Split-Path -Parent $PSScriptRoot) 'third_party\qtbase-v6.11.1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repo=Split-Path -Parent $PSScriptRoot
$patch=Join-Path $repo 'qt-patches\qtbase-6.11.1\0001-mvm-native-present-hook.patch'
if(-not(Test-Path -LiteralPath $patch)){throw "F3-C0 Qt patchがありません: $patch"}
if(-not(Test-Path -LiteralPath $SourceDirectory)){
    & git clone --branch v6.11.1 --depth 1 https://github.com/qt/qtbase.git $SourceDirectory
    if($LASTEXITCODE-ne0){throw 'QtBase v6.11.1 cloneに失敗しました'}
    & git -C $SourceDirectory apply $patch
    if($LASTEXITCODE-ne0){throw 'F3-C0 Qt patch適用に失敗しました'}
}
$upstream=(& git -C $SourceDirectory rev-parse HEAD).Trim()
if($LASTEXITCODE-ne0-or$upstream-ne'59c81a3c2247b821b9b84b4eb8d939b77e07e276'){
    throw "QtBase upstream commitが一致しません: $upstream"
}
& git -C $SourceDirectory apply --reverse --check $patch
if($LASTEXITCODE-ne0){throw 'QtBase sourceが固定patchと一致しません'}
Write-Host "F3-C0 Qt source: PASS ($upstream)"
