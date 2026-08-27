[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
$c1=Join-Path $Directory 'broken-c1.json';$c2=Join-Path $Directory 'c2.json'
@{schema='broken-c1'}|ConvertTo-Json|Set-Content -LiteralPath $c1 -Encoding utf8
@{source_c1_proof=$c1}|ConvertTo-Json|Set-Content -LiteralPath $c2 -Encoding utf8
$failed=$false;try{& $Checker -Proof $c2}catch{$failed=$true}
if(-not$failed){throw '壊れたC1 artifactをC2 checkerが受理しました'}
Write-Output 'W2-C2 broken C1 rejection: PASS'
