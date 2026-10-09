param([Parameter(Mandatory)][string]$Executable,
      [Parameter(Mandatory)][string]$OutputRoot,
      [ValidateSet('diagnostic', 'oracle', 'differential')][string]$Stage = 'differential')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$evidence = Join-Path $OutputRoot ('graph-composition-' + [guid]::NewGuid().ToString('N'))
& $Executable $Stage $evidence
exit $LASTEXITCODE
