#ifndef LLVM_POLYMORPHIC_BUILD_H
#define LLVM_POLYMORPHIC_BUILD_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class PolymorphicBuildPass : public PassInfoMixin<PolymorphicBuildPass> {
public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
    static bool isRequired() { return false; }
};

} // namespace llvm

#endif
