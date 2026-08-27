param(
    [Parameter(Mandatory=$true)][string]$Json,
    [int]$ProcessExitCode=3,
    [string]$VBlankChecker=(Join-Path $PSScriptRoot 'check-p2-vblank-shadow.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
# mapper failでprocessは3だが、physical authority自体は独立に0相当で検査する。
& pwsh -NoProfile -File $VBlankChecker -Json $Json -ProcessExitCode 0
if($LASTEXITCODE-ne0){Fail 'physical VBlank authorityも不成立です'}
$raw=Get-Content -LiteralPath $Json -Raw -Encoding utf8|ConvertFrom-Json
$mapper=$raw.presentation_opportunity.incremental_mapper_shadow
if($ProcessExitCode-ne3-or[int]$raw.process_exit_code-ne3){Fail 'mapper failのprocess exitは3でなければなりません'}
if($mapper.enabled-ne$true-or$mapper.shadow_only-ne$true-or$mapper.formal_counter_authority_changed-ne$false){Fail 'FAIL runがshadow-onlyではありません'}
if($mapper.mapper_pass-ne$false-or$mapper.mapper_error-ne'NO_SOLUTION'-or
   $mapper.final_solution_class-ne'NO_SOLUTION'){Fail '期待したNO_SOLUTION failではありません'}
$transitions=@($mapper.transitions)
if($transitions.Count-lt4){Fail 'incremental transitionが不足しています'}
$terminal=$transitions[-1]
if($terminal.event_type-ne'VBLANK'-or$terminal.mapper_error-ne'NO_SOLUTION'){Fail 'VBlank domain closeでNO_SOLUTIONになっていません'}
$previousVblank=-1
for($index=$transitions.Count-2;$index-ge0;--$index){if($transitions[$index].event_type-in@('VBLANK','ORIGIN')){$previousVblank=$index;break}}
if($previousVblank-lt0){Fail 'terminal直前のVBlankがありません'}
$callbacks=@($transitions[($previousVblank+1)..($transitions.Count-2)]|Where-Object{$_.event_type-eq'CALLBACK'})
if($callbacks.Count-lt2){Fail 'same-domain callback collisionが2件未満です'}
foreach($callback in $callbacks){
    if([int64]$callback.qpc-le[int64]$transitions[$previousVblank].qpc-or
       [int64]$callback.qpc-ge[int64]$terminal.qpc){Fail 'callbackが同じclosed VBlank domain内にありません'}}
$visibleOpportunityCount=[int64]$terminal.vblank_ordinal-[int64]$mapper.origin_vblank_ordinal
if([int64]$terminal.closed_record_count-le$visibleOpportunityCount){Fail 'injective capacity不足を再計算できません'}
Write-Host ("B1 shadow expected failure: PASS error=NO_SOLUTION closed=$($terminal.closed_record_count) " +
    "visible_opportunities=$visibleOpportunityCount same_domain_callbacks=$($callbacks.Count)")
