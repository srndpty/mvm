#ifndef MVM_PROJECT_EQUATION_BINDING_EDIT_H
#define MVM_PROJECT_EQUATION_BINDING_EDIT_H
#include "project/equation_sequence.h"

namespace mvm::project {
struct TrustedEquationEdit {
    std::string oldSource;
    std::size_t beginUtf16 = 0;
    std::size_t endUtf16 = 0;
    std::string replacement;
    std::string newSource;
};

// 挿入は begin では前、end では後。非空の編集は [begin,end) の交差で無効化する。
bool editEquationSourceTrusted(EquationSequenceClipData&, StateId, const TrustedEquationEdit&,
                               std::string revision, int outputHeight, std::string& error);
bool rebindEquationPart(EquationSequenceClipData&, StateId, PartId, SourceBinding, int outputHeight,
                        std::string& error);
} // namespace mvm::project
#endif
