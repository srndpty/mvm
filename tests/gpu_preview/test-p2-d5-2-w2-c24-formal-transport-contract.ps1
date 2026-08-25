[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativeDuplicateWithMissingMembershipProvenance',
        'NegativeDuplicateDispositionSwappedToOutside',
        'NegativeOutsideDispositionSwappedToDuplicate','NegativeEligibilityMutation')]
    [string]$Case,
    [Parameter(Mandatory=$true)][string]$Core
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. $Core
$records=@(
    [pscustomobject][ordered]@{
        token_serial='1';intent_ordinal='0';intent_scope='CURRENT_MEASUREMENT'
        required_current_membership=$true;required_current_membership_exact=$true
        duplicate_callback=$true;producer_semantics_exact=$true
        transport_disposition='SUPPRESS_DUPLICATE_CALLBACK';formal_transport_eligible=$false
    },
    [pscustomobject][ordered]@{
        token_serial='2';intent_ordinal='301';intent_scope='CURRENT_MEASUREMENT'
        required_current_membership=$false;required_current_membership_exact=$true
        duplicate_callback=$false;producer_semantics_exact=$true
        transport_disposition='SUPPRESS_OUTSIDE_REQUIRED_SET';formal_transport_eligible=$false
    },
    [pscustomobject][ordered]@{
        token_serial='3';intent_ordinal='7';intent_scope='CURRENT_MEASUREMENT'
        required_current_membership=$true;required_current_membership_exact=$true
        duplicate_callback=$false;producer_semantics_exact=$true
        transport_disposition='TRANSPORT';formal_transport_eligible=$true
    },
    [pscustomobject][ordered]@{
        token_serial='4';intent_ordinal='0';intent_scope='FOREIGN_PRE_MEASUREMENT'
        required_current_membership=$false;required_current_membership_exact=$true
        duplicate_callback=$false;producer_semantics_exact=$true
        transport_disposition='TRANSPORT';formal_transport_eligible=$true
    })
switch($Case){
    'NegativeDuplicateWithMissingMembershipProvenance'{$records[0].required_current_membership_exact=$false}
    'NegativeDuplicateDispositionSwappedToOutside'{$records[0].transport_disposition='SUPPRESS_OUTSIDE_REQUIRED_SET'}
    'NegativeOutsideDispositionSwappedToDuplicate'{$records[1].transport_disposition='SUPPRESS_DUPLICATE_CALLBACK'}
    'NegativeEligibilityMutation'{$records[2].formal_transport_eligible=$false}
}
$result=Invoke-MvmC24FormalTransportPolicy -ProducerRecords $records `
    -RecordedDuplicateSuppressedCount 1 -RecordedOutsideSuppressedCount 1
$good=$Case-eq'Good'
if($good-and-not[bool]$result.policy_exact){throw '正当なC2.4 policyをrejectしました'}
if(-not$good-and[bool]$result.policy_exact){throw "$Case をrejectしません"}
Write-Output "W2-C2.4 contract $Case`: PASS"
