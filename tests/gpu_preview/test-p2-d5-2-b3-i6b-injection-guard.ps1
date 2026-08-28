[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativeFatalInjectionAfterPublication','NegativePublicationGateIgnored',
        'NegativeSuppressionCounterMissing','NegativePostFatalReceiptTaken',
        'NegativePostFatalBindReached','NegativePostFatalCommitReached',
        'NegativePrimaryFatalOverwritten','NegativeQueueDequeuedAfterFatal',
        'NegativeFakeIdentityInjection','NegativeProductionComponentDuplicated')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Contract,
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$Directory,
    [Parameter(Mandatory=$true)][string]$GpuPreviewLibrary,
    [string]$Compiler='C:\msys64\ucrt64\bin\g++.exe'
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# B3-I6B deferred integration-negativeのmutation test。
# production headerかtest sourceを1点だけ壊し、architecture guardか実行結果のどちらかが
# 必ず落ちることを固定する。落ちなければacceptance chainが空振りしている。
$relatives=@(
    'src/app/preview/composition_token_publication.h',
    'src/app/preview/formal_present_join_admission.h',
    'src/app/preview/compositor_rhi_item.cpp',
    'tests/gpu_preview/test_i6b_publication_atomicity.cpp')
$mutationRoot=Join-Path $Directory "process-$PID"
$sources=@{}
foreach($relativePath in $relatives){
    $sources[$relativePath]=Get-Content -LiteralPath (Join-Path $SourceRoot $relativePath) -Raw -Encoding utf8
}
function Edit-Source([string]$RelativePath,[string]$From,[string]$To){
    $sourceText=$sources[$RelativePath]
    if($sourceText-notmatch[regex]::Escape($From)){throw "変異対象がありません: $RelativePath / $From"}
    $sources[$RelativePath]=$sourceText.Replace($From,$To)
}
$publication='src/app/preview/composition_token_publication.h'
$admission='src/app/preview/formal_present_join_admission.h'
$test='tests/gpu_preview/test_i6b_publication_atomicity.cpp'
switch($Case){
    'Good'{}
    'NegativeFatalInjectionAfterPublication'{
        Edit-Source $test '        sink.injectProtocolFatal("SOURCE_COVERAGE_INSUFFICIENT");
        publishCallsAtInjection = sink.publishCallCount;' '        publishCallsAtInjection = sink.publishCallCount;'
        Edit-Source $test '    check(publishCallsAtInjection == 0, "fatal注入時点で既にpublishされていました");' '    sink.injectProtocolFatal("SOURCE_COVERAGE_INSUFFICIENT");
    check(publishCallsAtInjection == 0, "fatal注入時点で既にpublishされていました");'}
    'NegativePublicationGateIgnored'{
        Edit-Source $publication 'return active_ && valid_ && !sink_.protocolFatalLatched();' 'return active_ && valid_;'}
    'NegativeSuppressionCounterMissing'{
        Edit-Source $publication '            sink_.noteSuppressedBeforePresent();' '            (void)0;'}
    'NegativePostFatalReceiptTaken'{
        Edit-Source $admission '        input.formalSchedulerEnabled && input.nativeCaptureActive && !input.protocolFatalLatched;' '        input.formalSchedulerEnabled && input.nativeCaptureActive;'}
    'NegativePostFatalBindReached'{
        Edit-Source $admission '        !input.protocolFatalLatched && !input.domainReached && input.formalCaptureActive;' '        !input.domainReached && input.formalCaptureActive;'}
    'NegativePostFatalCommitReached'{
        Edit-Source $test '    if (admission.joinCommitAllowed) {
        ++reach.bindCalls;
        ++reach.commitCalls;' '    ++reach.commitCalls;
    if (admission.joinCommitAllowed) {
        ++reach.bindCalls;'}
    'NegativePrimaryFatalOverwritten'{
        Edit-Source $test '        if (fatalReason == nullptr)
            fatalReason = reason;' '        fatalReason = reason;'}
    'NegativeQueueDequeuedAfterFatal'{
        Edit-Source $test '    const auto reach =' '    scheduler.commitQualifiedPresent(decision.reservationId, decision.opportunityOrdinal);
    const auto reach ='}
    'NegativeFakeIdentityInjection'{
        Edit-Source $test '    RecordingSink sink;
    const auto token = tokenWithSerial(3930);
    int publishCallsAtInjection = -1;' '    RecordingSink sink;
    auto token = tokenWithSerial(3930);
    const std::uint64_t fakePresentSerial = 4242;
    token.compositionEpoch = fakePresentSerial;
    int publishCallsAtInjection = -1;'}
    'NegativeProductionComponentDuplicated'{
        Edit-Source $test 'class RecordingSink final : public mvm::app::CompositionTokenPublicationSink {' 'class FakeTokenCapture {
public:
    bool publicationAllowed() const { return true; }
};

class RecordingSink final : public mvm::app::CompositionTokenPublicationSink {'}
}
foreach($relativePath in $relatives){
    $targetPath=Join-Path $mutationRoot $relativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $targetPath) -Force|Out-Null
    Set-Content -LiteralPath $targetPath -Value $sources[$relativePath] -Encoding utf8 -NoNewline
}

# 1. source-level contract
$guardFailed=$false
try{& $Contract -RepoRoot $mutationRoot *> $null}catch{$guardFailed=$true}

# 2. 変異したproduction header / testを実際にcompileして実行する
# UCRT64 のg++は依存DLLをPATHから解決する。PATHが整っていないと出力なしで失敗する。
$env:PATH="$(Split-Path -Parent $Compiler);$env:PATH"
$binary=Join-Path $mutationRoot 'i6b-injection.exe'
$mutatedTest=Join-Path $mutationRoot $test
$compileArguments=@('-std=c++20','-O1','-I',(Join-Path $mutationRoot 'src'),'-I',(Join-Path $SourceRoot 'src'),
    $mutatedTest,$GpuPreviewLibrary,'-o',$binary)
$compileOutput=& $Compiler @compileArguments 2>&1
$compileFailed=$LASTEXITCODE-ne0
$runFailed=$false
if(-not$compileFailed){
    & $binary *> $null
    $runFailed=$LASTEXITCODE-ne0
}
$detected=$guardFailed-or$compileFailed-or$runFailed
if($Case-eq'Good'){
    if($guardFailed){throw '未変異sourceをB3-I6B injection guardが拒否しました'}
    if($compileFailed){throw "未変異testをcompileできません (exit=$LASTEXITCODE): $compileOutput"}
    if($runFailed){throw '未変異testが失敗しました'}
    Write-Output "P2-D5-2 B3-I6B injection guard $Case`: PASS";exit 0
}
if(-not$detected){throw "$Case をB3-I6B injection guardが検出できませんでした"}
Write-Output "P2-D5-2 B3-I6B injection guard $Case`: PASS"
