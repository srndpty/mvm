param(
    [string]$PresentMonRoot=(Join-Path (Split-Path -Parent $PSScriptRoot) 'tmp\presentmon-r2-src'),
    [string]$Patch=(Join-Path (Split-Path -Parent $PSScriptRoot) 'presentmon-patches\2.3.1\0001-mvm-discard-reason-diagnostic.patch'),
    [string]$LifecyclePatch=(Join-Path (Split-Path -Parent $PSScriptRoot) 'presentmon-patches\2.3.1\0002-mvm-dependency-lifecycle-diagnostic.patch')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$expectedCommit='717c5bf14e80a4a06b70cd16415ae8d40a7ce201'
foreach($path in @($PresentMonRoot,$Patch,$LifecyclePatch)){if(-not(Test-Path -LiteralPath $path)){throw "PresentMon診断の必須pathがありません: $path"}}
$PresentMonRoot=(Resolve-Path -LiteralPath $PresentMonRoot).Path
$patches=@($Patch,$LifecyclePatch)|ForEach-Object{(Resolve-Path -LiteralPath $_).Path}
$actual=(& git -C $PresentMonRoot rev-parse HEAD).Trim()
if($LASTEXITCODE-ne0-or$actual-ne$expectedCommit){throw "PresentMon commitが固定値と一致しません: $actual"}
# 後段patchが前段patchのreverse contextを変えるため、stackを一度外して順番どおり再適用する。
foreach($diagnosticPatch in @($patches[1],$patches[0])){
    & git -C $PresentMonRoot apply --reverse --check $diagnosticPatch *> $null
    if($LASTEXITCODE-eq0){
        & git -C $PresentMonRoot apply --reverse $diagnosticPatch
        if($LASTEXITCODE-ne0){throw "PresentMon診断patchを一時解除できません: $diagnosticPatch"}
    }
}
foreach($diagnosticPatch in $patches){
    & git -C $PresentMonRoot apply --check $diagnosticPatch
    if($LASTEXITCODE-ne0){throw "PresentMon診断patch stackを再構成できません: $diagnosticPatch"}
    & git -C $PresentMonRoot apply $diagnosticPatch
    if($LASTEXITCODE-ne0){throw "PresentMon診断patchの適用に失敗しました: $diagnosticPatch"}
}
& git -C $PresentMonRoot diff --check
if($LASTEXITCODE-ne0){throw 'F3-C2 PresentMon診断patchにwhitespace errorがあります'}
$changed=@(& git -C $PresentMonRoot diff --name-only)
$unexpected=@($changed|Where-Object{$_-notin@('PresentData/PresentMonTraceConsumer.cpp','PresentData/PresentMonTraceConsumer.hpp')})
if($unexpected.Count-ne0){throw "pinned PresentMon sourceに想定外の変更があります: $($unexpected -join ', ')"}
Write-Host "F3-C2/A2 PresentMon diagnostic patches: PASS ($actual)"
