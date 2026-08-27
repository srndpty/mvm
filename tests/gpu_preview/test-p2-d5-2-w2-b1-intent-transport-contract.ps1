[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodFormal','GoodNonFormal','GoodSuppressedFormal','NegativeAppV4QtV5','NegativeAppV5QtV4',
        'NegativeTokenLayout','NegativeRecordLayout','NegativeLayoutSignature',
        'NegativeFormalInvalid','NegativeOrdinalMutation','NegativeValidityMutation',
        'NegativeDuplicateEmbeddedTokenSerial','NegativeDuplicateNativeSerial',
        'NegativeNonFormalFabricated','NegativeRingOverflow','NegativeMissingToken',
        'NegativeTokenSetFailure','NegativeAuthorityFailure','NegativeHookUnavailable',
        'NegativeLayoutHandshake','NegativeSecondProducer',
        'NegativeRequiredFormalModeFalse','NegativeSuppressionWithoutWitness',
        'NegativeSuppressionTransportDisposition')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
$formal=$Case-ne'GoodNonFormal'-and$Case-ne'NegativeNonFormalFabricated'-and
        $Case-ne'NegativeRequiredFormalModeFalse'
$records=@(0..2|ForEach-Object{
    [ordered]@{
        native_present_embedded_token_serial=[string](100+$_)
        composition_token_intent_ordinal=[string]$_
        composition_token_intent_valid=$formal
        native_present_serial=[string](200+$_)
        native_present_intent_ordinal=[string]$_
        native_present_intent_valid=$formal
        formal_transport_eligible=$formal
        suppression_exact=$false
        transport_disposition='TRANSPORT'
    }
})
$transport=[ordered]@{
    schema='mvm-p2-d5-2-w2-b1-intent-identity-transport-2'
    abi_version=5;app_abi_version=5;qt_abi_version_observed=5
    layout_handshake_accepted=$true;layout_signature='123456789'
    composition_token_size=120;native_present_record_size=200
    shadow_only=$true;performance_accounting_connected=$false;formal_mode=$formal
    record_count=$records.Count;transport_exact=$true
    verdict='INTENT_IDENTITY_ABI_V4_TRANSPORT_EXACT';records=$records
}
$hook=[ordered]@{
    abi_version=5;composition_token_size=120;native_present_record_size=200
    layout_signature='123456789';qt_abi_version_observed=5;layout_handshake_accepted=$true
    available=$true;hook_enabled=$true;capture_started=$true;capture_stopped=$true
    overflow_count=0;missing_token_count=0;duplicate_token_count=0;stale_token_count=0
    token_set_failure_count=0;failed_present_count=0;authority_failure=$false
    intent_identity_transport=$transport
}
$checkerSourceRoot=$SourceRoot
$requireFormalMode=$Case-eq'NegativeRequiredFormalModeFalse'
switch($Case){
    'GoodSuppressedFormal'{
        $records[1].composition_token_intent_valid=$false;$records[1].native_present_intent_valid=$false
        $records[1].formal_transport_eligible=$false;$records[1].suppression_exact=$true
        $records[1].transport_disposition='SUPPRESS_DUPLICATE_CALLBACK'
    }
    'NegativeAppV4QtV5'{$transport.app_abi_version=4}
    'NegativeAppV5QtV4'{$transport.qt_abi_version_observed=4;$hook.qt_abi_version_observed=4}
    'NegativeTokenLayout'{$transport.composition_token_size=119}
    'NegativeRecordLayout'{$transport.native_present_record_size=201}
    'NegativeLayoutSignature'{$transport.layout_signature='987654321'}
    'NegativeFormalInvalid'{$records[1].composition_token_intent_valid=$false;$records[1].native_present_intent_valid=$false}
    'NegativeOrdinalMutation'{$records[1].native_present_intent_ordinal='99'}
    'NegativeValidityMutation'{$records[1].native_present_intent_valid=$false}
    'NegativeDuplicateEmbeddedTokenSerial'{$records[1].native_present_embedded_token_serial=$records[0].native_present_embedded_token_serial}
    'NegativeDuplicateNativeSerial'{$records[1].native_present_serial=$records[0].native_present_serial}
    'NegativeNonFormalFabricated'{$records[1].composition_token_intent_valid=$true;$records[1].native_present_intent_valid=$true}
    'NegativeRingOverflow'{$hook.overflow_count=1}
    'NegativeMissingToken'{$hook.missing_token_count=1}
    'NegativeTokenSetFailure'{$hook.token_set_failure_count=1}
    'NegativeAuthorityFailure'{$hook.authority_failure=$true}
    'NegativeHookUnavailable'{$hook.available=$false}
    'NegativeLayoutHandshake'{$hook.layout_handshake_accepted=$false;$transport.layout_handshake_accepted=$false}
    'NegativeSecondProducer'{
        $checkerSourceRoot="$Output-architecture"
        foreach($relative in @('src/app/preview/native_present_hook_abi.h',
                               'src/app/preview/compositor_rhi_item.cpp',
                               'apps/compositor_spike/compositor_spike_controller.cpp',
                               'qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch')){
            $destination=Join-Path $checkerSourceRoot $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force|Out-Null
            Copy-Item -LiteralPath (Join-Path $SourceRoot $relative) -Destination $destination -Force
        }
        $rendererPath=Join-Path $checkerSourceRoot 'src/app/preview/compositor_rhi_item.cpp'
        $sourceText=Get-Content -LiteralPath $rendererPath -Raw -Encoding utf8
        $needle='nativePresentToken.setFormalIntentOrdinal(formalDecision.opportunityOrdinal)'
        $sourceText=$sourceText.Replace($needle,"$needle; nativePresentToken.setFormalIntentOrdinal(output)")
        Set-Content -LiteralPath $rendererPath -Value $sourceText -Encoding utf8
    }
    'NegativeSuppressionWithoutWitness'{
        $records[1].composition_token_intent_valid=$false;$records[1].native_present_intent_valid=$false
        $records[1].formal_transport_eligible=$false;$records[1].suppression_exact=$false
        $records[1].transport_disposition='SUPPRESS_DUPLICATE_CALLBACK'
    }
    'NegativeSuppressionTransportDisposition'{
        $records[1].composition_token_intent_valid=$false;$records[1].native_present_intent_valid=$false
        $records[1].formal_transport_eligible=$false;$records[1].suppression_exact=$true
        $records[1].transport_disposition='TRANSPORT'
    }
}
[ordered]@{native_present_hook=$hook}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $Output -Encoding utf8
if($requireFormalMode){
    & pwsh -NoProfile -File $Checker -Json $Output -SourceRoot $checkerSourceRoot `
        -RequireFormalMode *> $null
}else{
    & pwsh -NoProfile -File $Checker -Json $Output -SourceRoot $checkerSourceRoot *> $null
}
$actual=$LASTEXITCODE
$expected=if($Case-in@('GoodFormal','GoodNonFormal','GoodSuppressedFormal')){0}else{1}
if($actual-ne$expected){throw "$Case W2-B1 contract exitが不正です: expected=$expected actual=$actual"}
Write-Host "W2-B1 $Case contract: PASS"
