[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Proof)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c13-formal-population.ps1') -Proof $Proof
