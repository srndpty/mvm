[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('ExactAvailable','ExeMissing','QtGuiMissing','UnchangedFromCurrent',
                 'RebuildIsNotExact','NegativeMissingIdentities')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Inventory,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null
$searchRoot=Join-Path $Directory 'search'
New-Item -ItemType Directory -Path $searchRoot|Out-Null

# 実ファイルを置いてhashを実測させる。identitiesはその実測hashで組み立てる。
function New-Artifact([string]$Name,[string]$Content){
    $path=Join-Path $searchRoot $Name
    Set-Content -LiteralPath $path -Value $Content -Encoding ascii -NoNewline
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}
$exeHash=New-Artifact 'mvm_compositor_spike.exe' 'historical-exe'
$guiHash=New-Artifact 'Qt6Gui.dll' 'historical-gui'
$coreHash=New-Artifact 'Qt6Core.dll' 'shared-core'
$decoderHash=New-Artifact 'mvm_present_history_decoder.exe' 'shared-decoder'
$absentHash='0'*64

$historical=[ordered]@{executable_sha256=$exeHash;qt_gui_dll_sha256=$guiHash
    qt_core_dll_sha256=$coreHash;decoder_sha256=$decoderHash}
$current=[ordered]@{executable_sha256='c'*64;qt_gui_dll_sha256='d'*64
    qt_core_dll_sha256=$coreHash;decoder_sha256=$decoderHash}
switch($Case){
    'ExeMissing'          {$historical.executable_sha256=$absentHash}
    'QtGuiMissing'        {$historical.qt_gui_dll_sha256=$absentHash}
    'UnchangedFromCurrent'{$historical.executable_sha256='c'*64;$historical.qt_gui_dll_sha256='d'*64}
    'RebuildIsNotExact'   {
        # 「再ビルドした」体で別hashの実ファイルを置いても、historical hashには一致しない。
        $null=New-Artifact 'mvm_compositor_spike_rebuilt.exe' 'rebuilt-exe'
        $historical.executable_sha256=$absentHash
    }
}
$historicalPath=Join-Path $Directory 'historical-summary.json'
$currentPath=Join-Path $Directory 'current-summary.json'
if($Case-eq'NegativeMissingIdentities'){
    [ordered]@{submission_mode='CONTROL'}|ConvertTo-Json -Depth 4|
        Set-Content -LiteralPath $historicalPath -Encoding utf8
}else{
    [ordered]@{identities=$historical}|ConvertTo-Json -Depth 4|
        Set-Content -LiteralPath $historicalPath -Encoding utf8
}
[ordered]@{identities=$current}|ConvertTo-Json -Depth 4|
    Set-Content -LiteralPath $currentPath -Encoding utf8

$output=Join-Path $Directory 'proof.json'
$failed=$false
try{
    & pwsh -NoProfile -File $Inventory -HistoricalSummaryJson $historicalPath `
        -CurrentSummaryJson $currentPath -SearchRoot $searchRoot -Output $output
    if($LASTEXITCODE-ne0){$failed=$true}
}catch{$failed=$true}
if($Case-eq'NegativeMissingIdentities'){
    if(-not$failed){throw 'identitiesを欠くsummaryを受理しました'}
    Write-Host 'F3-C3-A3-T2-D1-B2 contract: PASS (NegativeMissingIdentities rejected)'
    exit 0
}
if($failed){throw "D1-B2 inventoryが失敗しました: $Case"}
$proof=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
$expected=switch($Case){
    'ExactAvailable'      {'EXACT_HISTORICAL_RUNTIME'}
    'UnchangedFromCurrent'{'EXACT_HISTORICAL_RUNTIME'}
    default               {'EXACT_HISTORICAL_RUNTIME_UNAVAILABLE'}
}
if([string]$proof.reconstruction_authority-ne$expected){
    throw "authorityが一致しません: expected=$expected actual=$($proof.reconstruction_authority)"
}
$exe=$proof.components|Where-Object component -eq 'mvm_compositor_spike.exe'
switch($Case){
    'ExactAvailable'      {if([string]$exe.availability-ne'EXACT_AVAILABLE'){throw "exe availabilityが不正です: $($exe.availability)"}}
    'UnchangedFromCurrent'{if([string]$exe.availability-ne'UNCHANGED_FROM_CURRENT'){throw "exe availabilityが不正です: $($exe.availability)"}}
    'ExeMissing'          {if([string]$exe.availability-ne'UNAVAILABLE'){throw "exe availabilityが不正です: $($exe.availability)"}}
    'RebuildIsNotExact'   {
        # 別hashのrebuild成果物が存在してもEXACTにはならない。
        if([string]$exe.availability-ne'UNAVAILABLE'){throw "rebuildをexactと誤認しました: $($exe.availability)"}
        if(@($exe.located_paths).Count-ne0){throw 'rebuild成果物をhistorical binaryとして紐付けました'}
    }
}
if($Case-eq'QtGuiMissing'){
    $gui=$proof.components|Where-Object component -eq 'Qt6Gui.dll'
    if([string]$gui.availability-ne'UNAVAILABLE'){throw "Qt6Gui availabilityが不正です: $($gui.availability)"}
    if(@($proof.missing_components)-notcontains'Qt6Gui.dll'){throw 'missing_componentsにQt6Gui.dllがありません'}
}
if([string]$proof.analysis_mode-ne'OFFLINE_HASH_SEARCH_NO_NEW_ACQUISITION'){throw 'analysis modeが不正です'}
Write-Host "F3-C3-A3-T2-D1-B2 contract: PASS ($Case -> $expected)"
