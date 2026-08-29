param(
    [Parameter(Mandatory=$true)][string]$Exe,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Output,
    [int]$Presents = 900,
    [switch]$DwmFlushFallback,
    # S2-e2: authority modeはCTest registrationが明示する。
    [ValidateSet('FULL_RELEASE','CORRECTNESS_ONLY')][string]$AuthorityMode='FULL_RELEASE'
)

$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Output) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    Copy-Item -LiteralPath $Output -Destination "$Output.previous-$stamp.json"
}
$arguments = @('--metrics', $Output, '--presents', $Presents,
               '--notify-delay-pattern', '0,100,300,800,1200',
               '--authority-mode', $AuthorityMode)
if ($DwmFlushFallback) { $arguments += '--dwm-flush-fallback' }
& $Exe @arguments
$probeExit = $LASTEXITCODE
if (-not (Test-Path -LiteralPath $Output)) {
    throw "probeがartifactを生成しませんでした: exit=$probeExit"
}
& pwsh -NoProfile -File $Checker -Json $Output -AuthorityMode $AuthorityMode
$checkerExit = $LASTEXITCODE
if ($probeExit -ne 0 -or $checkerExit -ne 0) {
    throw "完全なPresent-ID oracleを取得できませんでした: probe=$probeExit checker=$checkerExit"
}
