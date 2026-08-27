[CmdletBinding()]
param(
    # historical runのcanonical summary.json。identitiesを再構成目標とする。
    [Parameter(Mandatory=$true)][string]$HistoricalSummaryJson,
    # 現行runのcanonical summary.json。current runtimeの同定に使う。
    [Parameter(Mandatory=$true)][string]$CurrentSummaryJson,
    [string[]]$SearchRoot=@('C:\dev\soft\mvm'),
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Require($Object,[string]$Name){
    if($null-eq$Object-or-not($Object.PSObject.Properties.Name-contains$Name)){Fail "fieldがありません: $Name"}
    return $Object.$Name
}
foreach($path in @($HistoricalSummaryJson,$CurrentSummaryJson)){
    if(-not(Test-Path -LiteralPath $path)){Fail "summary jsonがありません: $path"}
}
$historical=Get-Content -LiteralPath $HistoricalSummaryJson -Raw -Encoding utf8|ConvertFrom-Json
$current=Get-Content -LiteralPath $CurrentSummaryJson -Raw -Encoding utf8|ConvertFrom-Json
$historicalIds=Require $historical 'identities'
$currentIds=Require $current 'identities'
# 探索対象component。file nameはbuild treeでの実名。
$components=@(
    @{key='executable_sha256';file='mvm_compositor_spike.exe'},
    @{key='qt_gui_dll_sha256';file='Qt6Gui.dll'},
    @{key='qt_core_dll_sha256';file='Qt6Core.dll'},
    @{key='decoder_sha256';file='mvm_present_history_decoder.exe'},
    @{key='t2_qt_quick_dll_sha256';file='Qt6Quick.dll'}
)
$fileNames=@($components|ForEach-Object{$_.file}|Sort-Object -Unique)
$found=@{}
foreach($root in $SearchRoot){
    if(-not(Test-Path -LiteralPath $root)){continue}
    Get-ChildItem -LiteralPath $root -Recurse -Include $fileNames -File -ErrorAction SilentlyContinue|
        ForEach-Object{
            $hash=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            if(-not$found.ContainsKey($hash)){$found[$hash]=@()}
            $found[$hash]+=$_.FullName
        }
}
$rows=@();$exactCount=0;$requiredCount=0
foreach($component in $components){
    $key=$component.key
    $historicalHash=if($historicalIds.PSObject.Properties.Name-contains$key){[string]$historicalIds.$key}else{$null}
    $currentHash=if($currentIds.PSObject.Properties.Name-contains$key){[string]$currentIds.$key}else{$null}
    # historicalが持たないcomponentは再構成対象ではない。
    if([string]::IsNullOrWhiteSpace($historicalHash)){
        $rows+=[pscustomobject][ordered]@{component=$component.file;identity_key=$key
            historical_sha256=$null;current_sha256=$currentHash
            availability='NOT_PART_OF_HISTORICAL_RUNTIME';located_paths=@()}
        continue
    }
    $requiredCount++
    # if/elseの空配列はunrollされて$nullになる。明示代入でarrayを保つ。
    $paths=@()
    if($found.ContainsKey($historicalHash)){$paths=@($found[$historicalHash])}
    $availability=if($paths.Count-gt0){'EXACT_AVAILABLE'}
                  elseif($historicalHash-eq$currentHash){'UNCHANGED_FROM_CURRENT'}
                  else{'UNAVAILABLE'}
    if($availability-eq'EXACT_AVAILABLE'-or$availability-eq'UNCHANGED_FROM_CURRENT'){$exactCount++}
    $rows+=[pscustomobject][ordered]@{component=$component.file;identity_key=$key
        historical_sha256=$historicalHash;current_sha256=$currentHash
        availability=$availability
        located_paths=@($paths|ForEach-Object{$_.Replace('C:\dev\soft\mvm\','')})}
}
# 全componentがexactに揃わない限りEXACT_HISTORICAL_RUNTIMEとは呼ばない。
$reconstruction=if($requiredCount-gt0-and$exactCount-eq$requiredCount){'EXACT_HISTORICAL_RUNTIME'}
                else{'EXACT_HISTORICAL_RUNTIME_UNAVAILABLE'}
$missing=@($rows|Where-Object availability -eq 'UNAVAILABLE'|ForEach-Object{$_.component})
$nextAction=if($reconstruction-eq'EXACT_HISTORICAL_RUNTIME'){'T2_D1_B2B_EXACT_HISTORICAL_VS_CURRENT_PROBE'}
            else{'DECIDE_REBUILD_SCOPE_OR_DEPRIORITIZE_ARCHAEOLOGY'}
# 現在のOS/driver/display provenanceを固定する。historical取得当時の値ではない。
$environment=[ordered]@{
    note='historical binaryを現在動かす実験の結果はcurrent OS/driver/desktop下のものであり、historical取得当時の完全再現ではない。'
    windows_build=[string](Get-CimInstance Win32_OperatingSystem).BuildNumber
    windows_caption=[string](Get-CimInstance Win32_OperatingSystem).Caption
    gpu=@(Get-CimInstance Win32_VideoController|ForEach-Object{
        [ordered]@{name=[string]$_.Name;driver_version=[string]$_.DriverVersion
            driver_date=[string]$_.DriverDate
            current_horizontal_resolution=[int]$_.CurrentHorizontalResolution
            current_vertical_resolution=[int]$_.CurrentVerticalResolution
            current_refresh_rate=[int]$_.CurrentRefreshRate}})
}
[ordered]@{
    schema='mvm-p2-c3-a3-t2-d1b2-runtime-reconstruction-1';status='PASS';authority='diagnostic_only'
    analysis_mode='OFFLINE_HASH_SEARCH_NO_NEW_ACQUISITION'
    reconstruction_authority=$reconstruction;next_action=$nextAction
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
    production_scheduler_changed=$false
    note='source commitからのrebuildはhash一致しない限りHISTORICAL_SOURCE_REBUILDであり、exact historical runtimeと同一視しない。'
    historical_summary=(Resolve-Path -LiteralPath $HistoricalSummaryJson).Path
    current_summary=(Resolve-Path -LiteralPath $CurrentSummaryJson).Path
    search_roots=$SearchRoot
    required_component_count=$requiredCount;exact_component_count=$exactCount
    missing_components=$missing
    components=$rows
    environment_provenance=$environment
}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C3-A3-T2-D1-B2 runtime reconstruction: PASS authority=$reconstruction missing=$($missing -join ',')"
