[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('NotEvaluableAbiMismatch','EvaluableSameAbi','AbiMismatchWithHistoricalQt',
                 'NegativeBadProofSchema')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null

# ABI versionはgitのsource treeから読むため、合成repositoryを用意する。
$repository=Join-Path $Directory 'repo'
$abiDirectory=Join-Path $repository 'src\app\preview'
New-Item -ItemType Directory -Path $abiDirectory -Force|Out-Null
function Resolve-Git(){
    # ctestのtest環境ではPATHにgitが無いことがある。既知の場所へfallbackする。
    $command=Get-Command git -CommandType Application -ErrorAction SilentlyContinue
    if($null-ne$command){return $command.Source}
    foreach($candidate in @((Join-Path $env:ProgramFiles 'Git\cmd\git.exe'),
                            (Join-Path $env:ProgramFiles 'Git\mingw64\bin\git.exe'),
                            'C:\msys64\usr\bin\git.exe')){
        if(Test-Path -LiteralPath $candidate){return $candidate}
    }
    throw 'gitを解決できません'
}
$git=Resolve-Git
& $git -C $repository init --quiet 2>$null
if($LASTEXITCODE-ne0){throw 'git initに失敗しました'}
& $git -C $repository config user.email 'contract@example.invalid'
& $git -C $repository config user.name 'contract'
$abiPath=Join-Path $abiDirectory 'native_present_hook_abi.h'
function Commit([int]$Version,[string]$Message){
    # 同一ABI versionのcaseでも2つ目のcommitが空にならないようmarkerを入れる。
    Set-Content -LiteralPath $abiPath -Encoding ascii -Value @(
        '#ifndef MVM_ABI_H',
        "// marker: $Message",
        "inline constexpr unsigned MVM_NATIVE_PRESENT_HOOK_ABI_VERSION = $Version;",
        '#endif')
    & $git -C $repository add -A *> $null
    & $git -C $repository commit --quiet -m $Message *> $null
    if($LASTEXITCODE-ne0){throw "git commitに失敗しました: $Message"}
    return (& $git -C $repository rev-parse HEAD).Trim()
}
$historicalCommit=Commit 2 'historical'
$currentVersion=if($Case-eq'EvaluableSameAbi'){2}else{3}
$currentCommit=Commit $currentVersion 'current'

$qtAvailability=if($Case-eq'AbiMismatchWithHistoricalQt'){'EXACT_AVAILABLE'}else{'UNAVAILABLE'}
$schema=if($Case-eq'NegativeBadProofSchema'){'wrong-schema'}
        else{'mvm-p2-c3-a3-t2-d1b2-runtime-reconstruction-1'}
$proofPath=Join-Path $Directory 'reconstruction.json'
[ordered]@{schema=$schema
    components=@([ordered]@{component='Qt6Gui.dll';availability=$qtAvailability})}|
    ConvertTo-Json -Depth 6|Set-Content -LiteralPath $proofPath -Encoding utf8

$output=Join-Path $Directory 'evaluability.json'
$failed=$false
try{
    & pwsh -NoProfile -File $Checker -HistoricalCommit $historicalCommit -CurrentCommit $currentCommit `
        -ReconstructionProof $proofPath -RepositoryRoot $repository -Output $output
    if($LASTEXITCODE-ne0){$failed=$true}
}catch{$failed=$true}
if($Case-eq'NegativeBadProofSchema'){
    if(-not$failed){throw '不正なschemaのproofを受理しました'}
    Write-Host 'F3-C3-A3-T2-D1-B2 evaluability contract: PASS (NegativeBadProofSchema rejected)'
    exit 0
}
if($failed){throw "evaluability checkerが失敗しました: $Case"}
$result=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
$expected=switch($Case){
    'NotEvaluableAbiMismatch'    {'REBUILD_PROBE_NOT_EVALUABLE'}
    'EvaluableSameAbi'           {'REBUILD_PROBE_EVALUABLE'}
    'AbiMismatchWithHistoricalQt'{'REBUILD_PROBE_EVALUABLE'}
}
if([string]$result.verdict-ne$expected){throw "verdictが一致しません: expected=$expected actual=$($result.verdict)"}
if($Case-eq'NotEvaluableAbiMismatch'){
    if([string]$result.reason-ne'NATIVE_PRESENT_HOOK_ABI_MISMATCH_AND_HISTORICAL_QT_UNAVAILABLE'){
        throw "reasonが不正です: $($result.reason)"
    }
    if([string]$result.next_action-ne'END_PROACTIVE_ARCHAEOLOGY_RETURN_TO_FORMAL_AUTHORITY_WIRING'){
        throw "next_actionが不正です: $($result.next_action)"
    }
}
if($Case-eq'AbiMismatchWithHistoricalQt'-and[string]$result.reason-ne'ABI_MISMATCH_REQUIRES_HISTORICAL_QT_RUNTIME'){
    throw "reasonが不正です: $($result.reason)"
}
Write-Host "F3-C3-A3-T2-D1-B2 evaluability contract: PASS ($Case -> $expected)"
