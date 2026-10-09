[CmdletBinding()]
param([Parameter(Mandatory)][string]$Executable, [Parameter(Mandatory)][string]$OutputRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$evidence = Join-Path $OutputRoot ('graph-artifact-' + [guid]::NewGuid().ToString('N'))
& $Executable $evidence
exit $LASTEXITCODE
