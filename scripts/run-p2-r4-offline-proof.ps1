[CmdletBinding()]
param(
    [string]$CorpusDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'bench\results\f3-b0.6-r3-corpus-20260822-v2'),
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$MapperExecutable = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ucrt64-release\bin\mvm_p2_offline_mapping.exe')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$CorpusDirectory=(Resolve-Path -LiteralPath $CorpusDirectory).Path
$MapperExecutable=(Resolve-Path -LiteralPath $MapperExecutable).Path
$ucrt64Bin='C:\msys64\ucrt64\bin'
if(-not(Test-Path -LiteralPath $ucrt64Bin -PathType Container)){throw "UCRT64 binがありません: $ucrt64Bin"}
$env:PATH="$ucrt64Bin;$env:PATH"
$corpusIndexPath=Join-Path $CorpusDirectory 'corpus-index.json'
$corpusCheckPath=Join-Path $CorpusDirectory 'check-result.json'
foreach($path in @($corpusIndexPath,$corpusCheckPath,$MapperExecutable)){
    if(-not(Test-Path -LiteralPath $path)){throw "R4必須fileがありません: $path"}
}
$corpusCheck=Get-Content -LiteralPath $corpusCheckPath -Raw -Encoding utf8|ConvertFrom-Json
if($corpusCheck.corpus_status-ne'PASS'){throw 'R3 corpusがPASSではありません'}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存R4 artifactを上書きしません: $OutputDirectory"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$caseRoot=Join-Path $OutputDirectory 'cases';New-Item -ItemType Directory -Path $caseRoot|Out-Null
$corpus=Get-Content -LiteralPath $corpusIndexPath -Raw -Encoding utf8|ConvertFrom-Json
$outputs=@()
foreach($entry in @($corpus.cases)){
    $sourcePath=Join-Path $CorpusDirectory ([string]$entry.mapper_input).Replace('/','\')
    if((Hash $sourcePath)-ne$entry.mapper_input_sha256){throw "R3 mapper input hash不一致: $($entry.case_id)"}
    $source=Get-Content -LiteralPath $sourcePath -Raw -Encoding utf8|ConvertFrom-Json
    $samples=@($source.vblank_samples)
    $visibleSamples=@($samples|ForEach-Object{[ordered]@{ordinal=[int64]$_.ordinal;qpc=[int64]$_.qpc}})
    $callbacks=@($source.records|ForEach-Object{[ordered]@{
        submission_index=[int64]$_.submission_index
        synthetic_callback_qpc=[int64]$_.synthetic_callback_qpc
    }})
    $visible=[ordered]@{
        schema='mvm-p2-r4-visible-input-1';case_id=[string]$entry.case_id
        sync_interval=[int]$source.sync_interval
        measurement_start_qpc=[int64]$samples[0].qpc
        measurement_end_qpc_exclusive=[int64]$samples[-1].qpc
        vblank_samples=$visibleSamples;callbacks=$callbacks
    }
    $caseDirectory=Join-Path $caseRoot ([string]$entry.case_id)
    New-Item -ItemType Directory -Path $caseDirectory|Out-Null
    $visiblePath=Join-Path $caseDirectory 'visible-input.json'
    $resultPath=Join-Path $caseDirectory 'mapper-result.json'
    $visible|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $visiblePath -Encoding utf8
    & $MapperExecutable --input $visiblePath --output $resultPath
    if($LASTEXITCODE-ne0){throw "offline mapperが失敗しました: $($entry.case_id) exit=$LASTEXITCODE"}
    $outputs+=[ordered]@{
        case_id=[string]$entry.case_id
        visible_input="cases/$($entry.case_id)/visible-input.json"
        mapper_result="cases/$($entry.case_id)/mapper-result.json"
        visible_input_sha256=Hash $visiblePath;mapper_result_sha256=Hash $resultPath
    }
}
$run=[ordered]@{
    schema='mvm-p2-r4-offline-run-1';authority='diagnostic_only'
    mapper_proof_status='GENERATED_NOT_YET_CHECKED';mapper_changed=$true
    admissibility_relation='VISIBLE_PREFIX: opportunity_start_qpc <= synthetic_callback_qpc'
    forbidden_mapper_fields=@('DisplayedQPC','actual opportunity','PresentIds','original callback QPC',
        'synthetic delay','expected solution_class')
    corpus_index_sha256=Hash $corpusIndexPath;corpus_check_sha256=Hash $corpusCheckPath
    mapper_executable_sha256=Hash $MapperExecutable;case_count=$outputs.Count;cases=$outputs
}
$runPath=Join-Path $OutputDirectory 'run-index.json'
$run|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $runPath -Encoding utf8
$manifest=Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File|Sort-Object FullName|ForEach-Object{
    $relative=$_.FullName.Substring($OutputDirectory.Length+1).Replace('\','/')
    "$(Hash $_.FullName)  $relative"}
$manifest|Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.sha256') -Encoding ascii
Write-Host "R4 offline mapper run: GENERATED ($($outputs.Count) cases)"
