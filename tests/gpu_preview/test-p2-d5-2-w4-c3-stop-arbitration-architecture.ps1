[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [ValidateSet('Good','NegativeStopPublishSerialRelaxedOrdering',
        'NegativeStopSideEffectBeforeArbitrationClaim','NegativeUnclaimedExplicitStopWriter',
        'NegativeUnclaimedFatalWriter','NegativeUnclaimedFatalLatchSite',
        'NegativeArbitrationResetDuringMeasurement','NegativeSecondArbitrationResetSite',
        'NegativeInlineArbitrationCas','NegativeMissingSchedulerConfigEmit',
        'NegativeArbitrationResetAfterMeasurementStartPublication',
        'NegativeSecondArbitrationResetWriterInHeader')]
    [string]$Case='Good'
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
# source treeもこのscript自身も改行が混ざり得るので、mutationは常にLFで比較する。
function Lf([string]$Text){$Text-replace "`r`n","`n"}
function Read-Source([string]$Relative){Lf (Get-Content -LiteralPath (Join-Path $SourceRoot $Relative) -Raw -Encoding utf8)}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Deny([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}
$rendererHeader=Read-Source 'src/app/preview/compositor_rhi_item.h'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'
$controller=Read-Source 'apps/compositor_spike/compositor_spike_controller.cpp'
$schedulerHeader=Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.h'
$scheduler=Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.cpp'
switch($Case){
    'NegativeStopPublishSerialRelaxedOrdering' {
        $rendererHeader=$rendererHeader.Replace('compare_exchange_strong(expected, cause, std::memory_order_seq_cst)','compare_exchange_strong(expected, cause, std::memory_order_relaxed)')}
    'NegativeStopSideEffectBeforeArbitrationClaim' {
        $renderer=$renderer.Replace((Lf @'
                    stopClaim_ = claimStopCause(*state_, StopArbitration::DomainTerminal);
                    state_->formalOpportunityDomainReached.store(true, std::memory_order_release);
'@),(Lf @'
                    state_->formalOpportunityDomainReached.store(true, std::memory_order_release);
                    stopClaim_ = claimStopCause(*state_, StopArbitration::DomainTerminal);
'@))}
    'NegativeUnclaimedExplicitStopWriter' {
        $controller=$controller.Replace((Lf @'
                const StopClaimResult claim = claimStopCause(*state_, StopArbitration::ExplicitStop);
                explicitStopClaim_ = claim;
                state_->measurementStopRequested.store(true, std::memory_order_release);
'@),(Lf @'
                state_->measurementStopRequested.store(true, std::memory_order_release);
'@))}
    'NegativeUnclaimedFatalWriter' {
        $controller=$controller.Replace((Lf @'
        fatalStopClaim_ = claimStopCause(*state_, StopArbitration::Fatal);
        state_->measurementStopRequested.store(true, std::memory_order_release);
'@),(Lf @'
        state_->measurementStopRequested.store(true, std::memory_order_release);
'@))}
    'NegativeUnclaimedFatalLatchSite' {
        $renderer=$renderer.Replace((Lf @'
            claimStopCause(*state_, StopArbitration::Fatal);
'@),'')}
    'NegativeArbitrationResetDuringMeasurement' {
        $rendererHeader=$rendererHeader.Replace((Lf @'
    if (state.measurementIntervalActive.load(std::memory_order_seq_cst) ||
        state.measurementStartCaptured.load(std::memory_order_seq_cst))
        state.stopArbitrationResetDuringMeasurementCount.fetch_add(1, std::memory_order_seq_cst);
'@),'')}
    'NegativeSecondArbitrationResetSite' {
        $renderer=$renderer.Replace((Lf @'
    void finishMeasurement(long long callbackBegin) {
'@),(Lf @'
    void finishMeasurement(long long callbackBegin) {
        state_->stopArbitration.store(StopArbitration::None, std::memory_order_seq_cst);
'@))}
    'NegativeInlineArbitrationCas' {
        $controller=$controller.Replace((Lf @'
                const StopClaimResult claim = claimStopCause(*state_, StopArbitration::ExplicitStop);
'@),(Lf @'
                StopArbitration expected = StopArbitration::None;
                state_->stopArbitration.compare_exchange_strong(expected, StopArbitration::ExplicitStop, std::memory_order_seq_cst);
                const StopClaimResult claim = StopClaimResult{};
'@))}
    'NegativeMissingSchedulerConfigEmit' {
        $controller=$controller.Replace('{"scheduler_config",','{"scheduler_config_absent",')}
    'NegativeArbitrationResetAfterMeasurementStartPublication' {
        $controller=$controller.Replace((Lf @'
    resetStopArbitrationForMeasurement(*state_);
    // start requestはこのepoch初期化がすべて済んだ後で最後にpublishする。
    state_->measurementStartRequested.store(true, std::memory_order_release);
'@),(Lf @'
    state_->measurementStartRequested.store(true, std::memory_order_release);
    resetStopArbitrationForMeasurement(*state_);
'@))}
    'NegativeSecondArbitrationResetWriterInHeader' {
        $rendererHeader=$rendererHeader.Replace((Lf @'
inline void resetStopArbitrationForMeasurement(CompositorSpikeState& state) {
'@),(Lf @'
inline void clearStopArbitration(CompositorSpikeState& state) {
    state.stopArbitration.store(StopArbitration::None, std::memory_order_seq_cst);
}

inline void resetStopArbitrationForMeasurement(CompositorSpikeState& state) {
'@))}
}
try{
    # amend 4: 単一atomicのarbitrationとhelper
    Require $rendererHeader 'enum class StopArbitration \{[\s\S]*None = 0,[\s\S]*DomainTerminal,[\s\S]*PlannedWindowEnd,[\s\S]*ExplicitStop,[\s\S]*Fatal,[\s\S]*\};' 'StopArbitration enumがありません'
    Require $rendererHeader 'inline StopClaimResult claimStopCause\(CompositorSpikeState& state, StopArbitration cause\)' 'claim helperがありません'
    Require $rendererHeader 'compare_exchange_strong\(expected, cause, std::memory_order_seq_cst\)' 'arbitration claimがseq_cst CASではありません'
    Require $rendererHeader 'explicitStopPublishSerial\.fetch_add\(1, std::memory_order_seq_cst\)' 'explicit stop publication serialがseq_cstではありません'
    Require $rendererHeader 'fatalPublishSerial\.fetch_add\(1, std::memory_order_seq_cst\)' 'fatal publication serialがseq_cstではありません'
    Require $rendererHeader 'result\.succeeded =[\s\S]{0,200}compare_exchange_strong[\s\S]{0,400}fetch_add\(1, std::memory_order_seq_cst\)' 'claim -> serialの順序がhelper内で固定されていません'

    # lifecycle: reset siteは1箇所、measurement中resetを検出する
    Require $rendererHeader 'inline void resetStopArbitrationForMeasurement\(CompositorSpikeState& state\)' 'lifecycle reset helperがありません'
    Require $rendererHeader 'stopArbitrationResetDuringMeasurementCount\.fetch_add\(1, std::memory_order_seq_cst\)' 'measurement中resetの検出がありません'
    $resetSites=([regex]::Matches($renderer+$controller,'resetStopArbitrationForMeasurement\(')).Count
    if($resetSites-ne1){throw "lifecycle reset siteが1箇所ではありません: $resetSites"}
    # reset writerの実体はheader内にあるので、header自身でexactly-oneを閉じる。
    $headerResetWriters=([regex]::Matches($rendererHeader,'stopArbitration\.store\(StopArbitration::None')).Count
    if($headerResetWriters-ne1){throw "arbitration reset writerが1箇所ではありません: $headerResetWriters"}
    Require $rendererHeader 'inline void resetStopArbitrationForMeasurement\(CompositorSpikeState& state\) \{[^}]*stopArbitration\.store\(StopArbitration::None' '唯一のreset writerがlifecycle helper内にありません'
    # reset は measurement start publication より前（同一thread上でsequenced-before）。
    Require $controller 'resetStopArbitrationForMeasurement\(\*state_\);[\s\S]{0,300}measurementStartRequested\.store\(true' 'arbitration resetがmeasurement start publicationより後にあります'
    Deny $controller 'measurementStartRequested\.store\(true[\s\S]{0,300}resetStopArbitrationForMeasurement\(\*state_\);' 'measurement start publication後にarbitration resetが走り得ます'
    Deny $renderer 'stopArbitration\.store\(' 'render側にarbitration resetがあります'
    Deny $controller 'stopArbitration\.store\(' 'controller側にhelper外のarbitration resetがあります'
    Deny $renderer 'stopArbitration\.compare_exchange_strong' 'helper外のinline CASがあります'
    Deny $controller 'stopArbitration\.compare_exchange_strong' 'helper外のinline CASがあります'

    # classified publication siteが全てclaimを通る
    Require $controller 'claimStopCause\(\*state_, StopArbitration::ExplicitStop\);[\s\S]{0,200}measurementStopRequested\.store\(true' 'explicit stop writerがclaimを通りません'
    Require $controller 'claimStopCause\(\*state_, StopArbitration::Fatal\);[\s\S]{0,200}measurementStopRequested\.store\(true' 'fatal shutdown由来のstop writerがclaimを通りません'
    Require $renderer 'claimStopCause\(\*state_, StopArbitration::Fatal\);[\s\S]{0,400}fatal\.store\(true' 'fatal latch siteがclaimを通りません'
    $domainReachedStores=([regex]::Matches($renderer,'formalOpportunityDomainReached\.store\(true')).Count
    $claimedDomainReached=([regex]::Matches($renderer,'claimStopCause\(\*state_, StopArbitration::DomainTerminal\);\s*state_->formalOpportunityDomainReached\.store\(true')).Count
    if($domainReachedStores-eq0-or$claimedDomainReached-ne$domainReachedStores){
        throw "DOMAIN_TERMINAL claimがside effectより前にないsiteがあります: claimed=$claimedDomainReached stores=$domainReachedStores"
    }
    Require $renderer 'claimStopCause\(\*state_, StopArbitration::PlannedWindowEnd\);[\s\S]{0,200}finishMeasurement\(callbackBegin\)' 'planned window end siteがclaimを通りません'
    $terminalClaims=([regex]::Matches($renderer,'claimStopCause\(\*state_, StopArbitration::DomainTerminal\)')).Count
    if($terminalClaims-ne2){throw "terminal branchのclaim siteが2箇所ではありません: $terminalClaims"}
    $fatalStores=([regex]::Matches($renderer,'fatal\.store\(true')).Count
    $fatalClaims=([regex]::Matches($renderer,'claimStopCause\(\*state_, StopArbitration::Fatal\)')).Count
    if($fatalClaims-lt$fatalStores){throw "fatal latch siteにclaimを通らないものがあります: claims=$fatalClaims stores=$fatalStores"}

    # amend 2: replay入力はscheduler instance config
    Require $schedulerHeader 'PresentationOpportunityConfig config;' 'snapshotがscheduler instance configを持ちません'
    Require $scheduler 'invocationRecords_,[\s\S]{0,40}config_\};' 'snapshotがconfig_をそのまま返しません'
    Require $controller '\{"scheduler_config",[\s\S]{0,900}source_frame_offset[\s\S]{0,900}required_frame_count' 'scheduler_configがemitされません'
    Require $controller 'formalOpportunitySnapshot\.config\.sourceFrameOffset' 'scheduler_configがinstance config由来ではありません'

    if($Case-ne'Good'){throw "mutationが検出されませんでした: $Case"}
}catch{
    if($Case-eq'Good'-or$_.Exception.Message-like'mutationが検出されませんでした:*'){throw}
}
Write-Output "W4-C3 stop arbitration architecture: PASS ($Case)"
