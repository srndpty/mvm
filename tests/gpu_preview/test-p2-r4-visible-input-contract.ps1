param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativeOriginalCallback','NegativeSyntheticDelay','NegativeExpectedClass',
        'NegativeDisplayed')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Exe,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force -Path $Directory|Out-Null
$contractInput=[ordered]@{schema='mvm-p2-r4-visible-input-1';case_id='contract';sync_interval=1
    measurement_start_qpc=100;measurement_end_qpc_exclusive=300
    vblank_samples=@([ordered]@{ordinal=10;qpc=100},[ordered]@{ordinal=11;qpc=200},[ordered]@{ordinal=12;qpc=300})
    callbacks=@([ordered]@{submission_index=0;synthetic_callback_qpc=250},
                [ordered]@{submission_index=1;synthetic_callback_qpc=260})}
switch($Case){
    'NegativeOriginalCallback'{$contractInput.callbacks[0].original_callback_qpc=150}
    'NegativeSyntheticDelay'{$contractInput.callbacks[0].synthetic_delay_ticks=100}
    'NegativeExpectedClass'{$contractInput.expected_solution_class='UNIQUE'}
    'NegativeDisplayed'{$contractInput.callbacks[0].displayed=@([ordered]@{qpc=100})}
}
$inputPath=Join-Path $Directory 'input.json';$outputPath=Join-Path $Directory 'output.json'
$contractInput|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $inputPath -Encoding utf8
& $Exe --input $inputPath --output $outputPath *> $null
$exitCode=$LASTEXITCODE
if($Case-eq'Good'){
    if($exitCode-ne0){throw "visible対照群が失敗しました: $exitCode"}
    $result=Get-Content -LiteralPath $outputPath -Raw|ConvertFrom-Json
    if($result.solution_class-ne'UNIQUE'-or@($result.assignment).Count-ne2){throw 'visible対照群の解が不正です'}
}elseif($exitCode-ne3){throw "禁止fieldの期待exitは3です: actual=$exitCode"}

