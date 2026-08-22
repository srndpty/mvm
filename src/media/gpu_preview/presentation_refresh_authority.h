#ifndef MVM_GPU_PREVIEW_PRESENTATION_REFRESH_AUTHORITY_H
#define MVM_GPU_PREVIEW_PRESENTATION_REFRESH_AUTHORITY_H

namespace mvm::gpu {

// P2-Q5で確立したDWM VBlank authorityのsample。refreshCountはcRefresh、
// qpcVBlankは同じsampleのqpcVBlankで、両者は同一のDwmGetCompositionTimingInfo
// 呼び出しから取る。
struct PresentationAuthoritySample {
    bool available = false;
    unsigned long long refreshCount = 0;
    long long qpcVBlank = 0;
    long long refreshNumerator = 0;
    long long refreshDenominator = 0;
};

// presentation opportunityの一次authorityはrefresh countであり、QPC間隔ではない。
// ordinalは「originのrefresh countからのrefresh数」そのもので、丸めも+1も入れない。
// QPC差分をopportunity間隔へ丸める規則をここへ持ち込んではいけない。
bool presentationAuthorityUsable(const PresentationAuthoritySample& value,
                                 long long refreshNumerator, long long refreshDenominator);

// 同一authority上でsampleが後退していないこと。refresh countとqpcVBlankの
// 双方が単調非減少である必要がある。
bool presentationAuthorityMonotonic(const PresentationAuthoritySample& earlier,
                                    const PresentationAuthoritySample& later);

// ordinal = sample.refreshCount - originRefreshCount。origin以前のsampleは
// 帰属できないのでfalseを返す。
bool presentationOpportunityOrdinal(unsigned long long originRefreshCount,
                                    const PresentationAuthoritySample& sample, long long& ordinal);

} // namespace mvm::gpu

#endif
