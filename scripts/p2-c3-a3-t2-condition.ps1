[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)]
    [ValidateSet('CONTROL','TARGET_RHIITEM_PIXEL_TOGGLE','EXTERNAL_DIRTY',
                 'TARGET_HWND_INVALIDATE','TARGET_HWND_REDRAW_NOW')][string]$Condition,
    [ValidateRange(12,300)][int]$WarmupSeconds=12,
    [ValidateRange(1,300)][int]$MeasureSeconds=15,
    [ValidateRange(30,600)][int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$runner=Join-Path $PSScriptRoot 'p2-c3-a3-t1-condition.ps1'
$checker=Join-Path $PSScriptRoot 'check-p2-c3-a3-t2-update-chain.ps1'
foreach($path in @($runner,$checker)){if(-not(Test-Path -LiteralPath $path)){throw "T2必須scriptがありません: $path"}}
$windowMode=switch($Condition){
    'EXTERNAL_DIRTY'{'VISIBLE_UNOCCLUDED_FORCE_DIRTY'}
    'TARGET_HWND_INVALIDATE'{'VISIBLE_UNOCCLUDED_TARGET_INVALIDATE'}
    'TARGET_HWND_REDRAW_NOW'{'VISIBLE_UNOCCLUDED_TARGET_REDRAW_NOW'}
    default{'VISIBLE_UNOCCLUDED'}
}
$dirtyMode=if($Condition-eq'TARGET_RHIITEM_PIXEL_TOGGLE'){'TARGET_RHIITEM_PIXEL_TOGGLE'}else{'CONTROL'}
& pwsh -NoProfile -File $runner -OutputDirectory $OutputDirectory -Mode $windowMode `
    -DirtyPropagationMode $dirtyMode -WarmupSeconds $WarmupSeconds `
    -MeasureSeconds $MeasureSeconds -TimeoutSeconds $TimeoutSeconds
if($LASTEXITCODE-ne0){throw "T2 $Condition canonical condition runが失敗しました"}
$app=Join-Path $OutputDirectory 'canonical\traced-app.json'
$proof=Join-Path $OutputDirectory 'update-chain-proof.json'
$expected=if($Condition-eq'TARGET_RHIITEM_PIXEL_TOGGLE'){'TARGET_PIXEL'}else{'CONTROL'}
& pwsh -NoProfile -File $checker -AppJson $app -ExpectedMode $expected -Output $proof
if($LASTEXITCODE-ne0){throw "T2 $Condition update chainが失敗しました"}
$conditionProof=Get-Content -LiteralPath (Join-Path $OutputDirectory 'condition-proof.json') -Raw -Encoding utf8|ConvertFrom-Json
$chainProof=Get-Content -LiteralPath $proof -Raw -Encoding utf8|ConvertFrom-Json
[ordered]@{
    schema='mvm-p2-c3-a3-t2-condition-run-1';status='PASS';authority='diagnostic_only'
    condition=$Condition;window_mode=$windowMode;dirty_propagation_mode=$dirtyMode
    update_chain=$chainProof;dwm_condition=$conditionProof
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
    production_scheduler_changed=$false
}|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $OutputDirectory 't2-summary.json') -Encoding utf8
Write-Host "F3-C3-A3-T2 condition: PASS condition=$Condition ($OutputDirectory)"
