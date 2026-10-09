param([Parameter(Mandatory)][string]$Executable,
      [Parameter(Mandatory)][string]$OutputRoot,
      [switch]$Encode)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$evidence = Join-Path $OutputRoot ('graph-export-' + [guid]::NewGuid().ToString('N'))
if ($Encode) { & $Executable --encode $evidence }
else { & $Executable $evidence }
exit $LASTEXITCODE
