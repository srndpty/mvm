param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativeUnknown','NegativeLost','NegativeFrameRange',
        'NegativeOpportunityOrder','NegativeDisplayedPayload')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
$appPath=Join-Path $Directory 'app.json';$oraclePath=Join-Path $Directory 'oracle.json'
$outputPath=Join-Path $Directory 'proof.json'
$app=[ordered]@{required_measurement_frame_count=6}
$records=@(
    [ordered]@{completion_class='PRESENTED';output_frame=0;present_serial='1';composition_token_serial='1';displayed_qpc=@(100);actual_opportunity_ordinals=@(10)},
    [ordered]@{completion_class='DISCARDED';output_frame=1;present_serial='2';composition_token_serial='2';displayed_qpc=@();actual_opportunity_ordinals=@()},
    [ordered]@{completion_class='PRESENTED';output_frame=2;present_serial='3';composition_token_serial='3';displayed_qpc=@(200);actual_opportunity_ordinals=@(20)},
    [ordered]@{completion_class='PRESENTED';output_frame=2;present_serial='4';composition_token_serial='4';displayed_qpc=@(300);actual_opportunity_ordinals=@(30)},
    [ordered]@{completion_class='DISCARDED';output_frame=4;present_serial='5';composition_token_serial='5';displayed_qpc=@();actual_opportunity_ordinals=@()},
    [ordered]@{completion_class='PRESENTED';output_frame=5;present_serial='6';composition_token_serial='6';displayed_qpc=@(400);actual_opportunity_ordinals=@(40)}
)
$oracle=[ordered]@{
    schema='mvm-p2-c0-native-etw-oracle-1';oracle_status='ORACLE_VALID'
    display_completion_status='CLOSED';native_present_count=6;discarded_count=2
    incomplete_unknown_count=0;lost_count=0;records=$records
}
switch($Case){
    'NegativeUnknown'{$oracle.incomplete_unknown_count=1;$oracle.display_completion_status='INCOMPLETE'}
    'NegativeLost'{$oracle.lost_count=1;$oracle.display_completion_status='INCOMPLETE'}
    'NegativeFrameRange'{$records[5].output_frame=6}
    'NegativeOpportunityOrder'{$records[3].actual_opportunity_ordinals=@(20)}
    'NegativeDisplayedPayload'{$records[1].displayed_qpc=@(150);$records[1].actual_opportunity_ordinals=@(15)}
}
$app|ConvertTo-Json -Depth 4|Set-Content -LiteralPath $appPath -Encoding utf8
$oracle|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $oraclePath -Encoding utf8
& pwsh -NoProfile -File $Checker -AppJson $appPath -OracleJson $oraclePath -Output $outputPath *> $null
$actual=$LASTEXITCODE;$expected=if($Case-eq'Good'){0}else{1}
if($actual-ne$expected){throw "$Case F3-C1 contract exitが不正です: expected=$expected actual=$actual"}
if($Case-eq'Good'){
    $proof=Get-Content -LiteralPath $outputPath -Raw -Encoding utf8|ConvertFrom-Json
    if(-not$proof.source_frame_accounting_exact-or$proof.displayed_unique_source_frames-ne3-or
       $proof.formal_source_frame_drops-ne3-or$proof.discarded_present_count_added_to_formal_drops){
        throw 'F3-C1 good proofのsource accountingが不正です'
    }
}
Write-Host "F3-C1 display authority $Case test: PASS"
