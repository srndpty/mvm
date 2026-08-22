param(
    [string]$PresentMonRoot=(Join-Path (Split-Path -Parent $PSScriptRoot) 'tmp\presentmon-r2-src'),
    [string]$Patch=(Join-Path (Split-Path -Parent $PSScriptRoot) 'presentmon-patches\2.3.1\0001-mvm-discard-reason-diagnostic.patch')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$expectedCommit='717c5bf14e80a4a06b70cd16415ae8d40a7ce201'
foreach($path in @($PresentMonRoot,$Patch)){if(-not(Test-Path -LiteralPath $path)){throw "F3-C2必須pathがありません: $path"}}
$PresentMonRoot=(Resolve-Path -LiteralPath $PresentMonRoot).Path;$Patch=(Resolve-Path -LiteralPath $Patch).Path
$actual=(& git -C $PresentMonRoot rev-parse HEAD).Trim()
if($LASTEXITCODE-ne0-or$actual-ne$expectedCommit){throw "PresentMon commitが固定値と一致しません: $actual"}
& git -C $PresentMonRoot apply --reverse --check $Patch *> $null
if($LASTEXITCODE-ne0){
    & git -C $PresentMonRoot apply --check $Patch
    if($LASTEXITCODE-ne0){throw 'F3-C2 PresentMon診断patchを適用できません'}
    & git -C $PresentMonRoot apply $Patch
    if($LASTEXITCODE-ne0){throw 'F3-C2 PresentMon診断patchの適用に失敗しました'}
}
& git -C $PresentMonRoot diff --check
if($LASTEXITCODE-ne0){throw 'F3-C2 PresentMon診断patchにwhitespace errorがあります'}
$changed=@(& git -C $PresentMonRoot diff --name-only)
$unexpected=@($changed|Where-Object{$_-notin@('PresentData/PresentMonTraceConsumer.cpp','PresentData/PresentMonTraceConsumer.hpp')})
if($unexpected.Count-ne0){throw "pinned PresentMon sourceに想定外の変更があります: $($unexpected -join ', ')"}
Write-Host "F3-C2 PresentMon discard reason diagnostic patch: PASS ($actual)"
