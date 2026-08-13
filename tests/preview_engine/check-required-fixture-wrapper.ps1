[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Wrapper,
    [Parameter(Mandatory)][string]$PresentFile,
    [Parameter(Mandatory)][string]$MissingFile
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$hint = 'pwsh scripts/make-p3-fixture.ps1'
$missingOutput = & pwsh -NoProfile -File $Wrapper -RequiredFile $MissingFile -Hint $hint `
    -ProbeOnly 2>&1
$missingExit = $LASTEXITCODE
if ($missingExit -ne 2) {
    throw "fixture欠如がexit 2になりませんでした: $missingExit"
}
if (($missingOutput -join "`n") -notmatch [regex]::Escape($hint)) {
    throw 'fixture欠如メッセージに公式生成コマンドがありません'
}

& pwsh -NoProfile -File $Wrapper -RequiredFile $PresentFile -Hint $hint -ProbeOnly
if ($LASTEXITCODE -ne 0) {
    throw "fixture存在時のpreflightが失敗しました: exit $LASTEXITCODE"
}

Write-Host 'PASS: 必須fixture wrapperのmissing/present contract'
