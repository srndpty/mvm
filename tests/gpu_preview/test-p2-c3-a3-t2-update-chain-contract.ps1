[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('Good','NegativeMissingStage','NegativePreflight')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null
$app=Join-Path $Directory 'app.json';$proof=Join-Path $Directory 'proof.json'
$names=@('renderer_update','node_schedule_update','window_update','node_render',
         'compositor_render','composition_token','dirty_material','texture_changed',
         'qsg_main_render','rhi_end_frame','successful_present','target_pixel_toggle')
$counts=[ordered]@{};foreach($name in $names){$counts[$name]='0'}
$records=@();$presents=@()
foreach($serial in 1..62){
    $q=[ordered]@{};foreach($name in $names){$q[$name]=0}
    $q.renderer_update=$serial*100+1;$q.node_schedule_update=$serial*100+2
    $q.window_update=$serial*100+3
    $counts.renderer_update=[string]([int64]$counts.renderer_update+1)
    $counts.node_schedule_update=[string]([int64]$counts.node_schedule_update+1)
    $counts.window_update=[string]([int64]$counts.window_update+1)
    if($serial-le61){
        $q.node_render=$serial*100+4;$q.compositor_render=$serial*100+5
        $counts.node_render=[string]([int64]$counts.node_render+1)
        $counts.compositor_render=[string]([int64]$counts.compositor_render+1)
    }
    if($serial-le60){
        $stage=6
        foreach($name in @('composition_token','dirty_material','texture_changed','qsg_main_render',
                            'rhi_end_frame','successful_present')){
            $q[$name]=$serial*100+$stage;$stage++
            $counts[$name]=[string]([int64]$counts[$name]+1)
        }
        $presents+=[ordered]@{
            present_serial=[string]$serial;propagation_serial=[string]$serial
            composition_token=[ordered]@{propagation_serial=[string]$serial;token_serial=[string](1000+$serial)}
        }
    }
    $records+=[ordered]@{
        propagation_serial=[string]$serial
        composition_token_serial=$(if($serial-le60){[string](1000+$serial)}else{'0'})
        present_serial=$(if($serial-le60){[string]$serial}else{'0'})
        output_frame=$(if($serial-le60){$serial-1}else{0});stage_qpc=$q
    }
}
$presents+=@(
    [ordered]@{present_serial='61';propagation_serial='0';composition_token=[ordered]@{propagation_serial='0';token_serial='0'}},
    [ordered]@{present_serial='62';propagation_serial='0';composition_token=[ordered]@{propagation_serial='0';token_serial='0'}})
if($Case-eq'NegativeMissingStage'){$records[9].stage_qpc.texture_changed=0}
$preflight=[ordered]@{
    target_hwnd='0x1234';gwl_exstyle_raw='256';QT_QPA_DISABLE_REDIRECTION_SURFACE=$null
    QT_D3D_NO_FLIP=$null;QT_D3D_MAX_FRAME_LATENCY='2';QSG_NO_VSYNC=$null
}
if($Case-eq'NegativePreflight'){$preflight.target_hwnd=''}
[ordered]@{
    measurement_available=$true;measurement_present_callback_count=62
    measurement_elapsed_seconds=1.0;effective_fps=60.0
    diagnostic_target_rhiitem_pixel_toggle=$false;t2_preflight=$preflight
    native_present_hook=[ordered]@{
        authority_pass=$true;authority_failure=$false;records=$presents
        dirty_propagation=[ordered]@{
            schema='mvm-p2-c3-a3-t2-dirty-propagation-1';overflow_count=0
            duplicate_stage_count=0;record_count=62;stage_counts=$counts;records=$records
        }
    }
}|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $app -Encoding utf8
$process=Start-Process -FilePath 'pwsh' -ArgumentList @('-NoProfile','-File',$Checker,
    '-AppJson',$app,'-ExpectedMode','CONTROL','-Output',$proof) -Wait -PassThru -WindowStyle Hidden
$expected=if($Case-eq'Good'){0}else{1}
if($process.ExitCode-ne$expected){throw "T2 contract exitが不正です: case=$Case expected=$expected actual=$($process.ExitCode)"}
if($Case-eq'Good'){
    $result=Get-Content -LiteralPath $proof -Raw -Encoding utf8|ConvertFrom-Json
    if([string]$result.verdict-ne'UPDATE_CHAIN_EXACT'-or[int]$result.exact_closed_count-ne60){throw 'T2 good proofが不正です'}
}
Write-Host "F3-C3-A3-T2 $Case contract: PASS"
