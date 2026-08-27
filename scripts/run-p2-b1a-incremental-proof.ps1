[CmdletBinding()]
param(
    [string]$R4Directory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'bench\results\f3-b0.6-r4-offline-20260822-v3'),
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$MapperExecutable = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ucrt64-release\bin\mvm_p2_offline_mapping.exe')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$R4Directory=(Resolve-Path -LiteralPath $R4Directory).Path
$MapperExecutable=(Resolve-Path -LiteralPath $MapperExecutable).Path
$ucrt64Bin='C:\msys64\ucrt64\bin'
if(-not(Test-Path -LiteralPath $ucrt64Bin -PathType Container)){throw "UCRT64 binがありません: $ucrt64Bin"}
$env:PATH="$ucrt64Bin;$env:PATH"
$r4RunPath=Join-Path $R4Directory 'run-index.json'
$r4ProofPath=Join-Path $R4Directory 'proof-result.json'
foreach($path in @($r4RunPath,$r4ProofPath,$MapperExecutable)){
    if(-not(Test-Path -LiteralPath $path)){throw "B1a必須fileがありません: $path"}
}
$r4Proof=Get-Content -LiteralPath $r4ProofPath -Raw -Encoding utf8|ConvertFrom-Json
if($r4Proof.r4_status-ne'PASS'){throw 'R4 proofがPASSではありません'}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存B1a artifactを上書きしません: $OutputDirectory"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$caseRoot=Join-Path $OutputDirectory 'cases';New-Item -ItemType Directory -Path $caseRoot|Out-Null
$r4Run=Get-Content -LiteralPath $r4RunPath -Raw -Encoding utf8|ConvertFrom-Json
$outputs=@()
foreach($entry in @($r4Run.cases)){
    $visiblePath=Join-Path $R4Directory ([string]$entry.visible_input).Replace('/','\')
    $offlinePath=Join-Path $R4Directory ([string]$entry.mapper_result).Replace('/','\')
    if((Hash $visiblePath)-ne$entry.visible_input_sha256-or(Hash $offlinePath)-ne$entry.mapper_result_sha256){
        throw "R4 case hash不一致: $($entry.case_id)"}
    $caseDirectory=Join-Path $caseRoot ([string]$entry.case_id)
    New-Item -ItemType Directory -Path $caseDirectory|Out-Null
    $resultPath=Join-Path $caseDirectory 'incremental-result.json'
    & $MapperExecutable --input $visiblePath --output $resultPath --incremental
    if($LASTEXITCODE-ne0){throw "incremental mapperが失敗しました: $($entry.case_id) exit=$LASTEXITCODE"}
    $outputs+=[ordered]@{
        case_id=[string]$entry.case_id
        r4_visible_input=[string]$entry.visible_input
        r4_offline_result=[string]$entry.mapper_result
        incremental_result="cases/$($entry.case_id)/incremental-result.json"
        incremental_result_sha256=Hash $resultPath
    }
}
$run=[ordered]@{
    schema='mvm-p2-b1a-incremental-run-1';authority='diagnostic_only'
    b1a_status='GENERATED_NOT_YET_CHECKED';case_count=$outputs.Count
    r4_run_sha256=Hash $r4RunPath;r4_proof_sha256=Hash $r4ProofPath
    mapper_executable_sha256=Hash $MapperExecutable
    admissibility_relation='VISIBLE_PREFIX: opportunity_start_qpc <= synthetic_callback_qpc'
    replay_order='QPC_ASCENDING_VBLANK_BEFORE_CALLBACK_ON_TIE';cases=$outputs
}
$runPath=Join-Path $OutputDirectory 'run-index.json'
$run|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $runPath -Encoding utf8
$manifest=Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File|Sort-Object FullName|ForEach-Object{
    $relative=$_.FullName.Substring($OutputDirectory.Length+1).Replace('\','/')
    "$(Hash $_.FullName)  $relative"}
$manifest|Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.sha256') -Encoding ascii
Write-Host "B1a incremental replay: GENERATED ($($outputs.Count) cases)"
