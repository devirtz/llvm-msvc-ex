#include "PolymorphicBuild.h"
#include "CryptoUtils.h"
#include "Utils.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <cstdint>
#include <map>
#include <vector>

using namespace llvm;

static cl::opt<bool> RunPolyBuild(
    "poly-build",
    cl::NotHidden,
    cl::init(false),
    cl::desc("OLLVM - per-build constant polymorphism + fingerprint injection"));

static constexpr uint64_t kMinObfuscateValue = 2;
static constexpr unsigned kMaxBitWidth       = 64;

namespace {

struct PolymorphicBuild {
    CryptoUtils RNG;
    std::map<unsigned, GlobalVariable *> MaskGlobals;

    static bool shouldSplit(const ConstantInt *CI) {
        if (CI->getBitWidth() > kMaxBitWidth) return false;
        return CI->getZExtValue() >= kMinObfuscateValue;
    }

    GlobalVariable *getOrCreateMaskGlobal(Module &M, unsigned BitWidth) {
        auto It = MaskGlobals.find(BitWidth);
        if (It != MaskGlobals.end())
            return It->second;

        LLVMContext &Ctx = M.getContext();
        IntegerType *Ty  = IntegerType::get(Ctx, BitWidth);

        uint64_t Mask = RNG.get_uint64_t();
        if (BitWidth < 64)
            Mask &= (uint64_t(1) << BitWidth) - 1;

        GlobalVariable *GV = new GlobalVariable(
            M, Ty, /*isConstant=*/false,
            GlobalValue::PrivateLinkage,
            ConstantInt::get(Ty, Mask),
            "poly_mask_i" + std::to_string(BitWidth));

        MaskGlobals[BitWidth] = GV;
        return GV;
    }

    bool splitConstantsInFunction(Function &F, Module &M) {
        if (!toObfuscate(RunPolyBuild, &F, "poly-build"))
            return false;

        using Triple = std::tuple<Instruction *, unsigned, ConstantInt *>;
        std::vector<Triple> WorkList;

        for (BasicBlock &BB : F) {
            if (BB.isEHPad()) continue;
            for (Instruction &I : BB) {
                if (isa<PHINode>(&I)) continue;
                if (I.isTerminator()) continue;
                if (isa<GetElementPtrInst>(&I)) continue;
                if (auto *CI2 = dyn_cast<CallInst>(&I))
                    if (CI2->getCalledFunction() &&
                        CI2->getCalledFunction()->isIntrinsic()) continue;
                for (unsigned OpIdx = 0; OpIdx < I.getNumOperands(); ++OpIdx) {
                    if (auto *CI = dyn_cast<ConstantInt>(I.getOperand(OpIdx)))
                        if (shouldSplit(CI))
                            WorkList.emplace_back(&I, OpIdx, CI);
                }
            }
        }

        for (auto &[Inst, OpIdx, CI] : WorkList) {
            unsigned BitWidth = CI->getBitWidth();
            uint64_t OrigVal  = CI->getZExtValue();

            GlobalVariable *MaskGV  = getOrCreateMaskGlobal(M, BitWidth);
            uint64_t        MaskVal = cast<ConstantInt>(MaskGV->getInitializer())->getZExtValue();
            uint64_t        Encoded = OrigVal ^ MaskVal;

            IntegerType *Ty = IntegerType::get(M.getContext(), BitWidth);
            IRBuilder<>  IRB(Inst);

            Value *LoadedMask = IRB.CreateLoad(Ty, MaskGV, "poly_m");
            Value *EncodedC   = ConstantInt::get(Ty, Encoded);
            Value *Decoded    = IRB.CreateXor(EncodedC, LoadedMask, "poly_c");

            Inst->setOperand(OpIdx, Decoded);
        }

        return !WorkList.empty();
    }

    void injectFingerprint(Module &M) {
        LLVMContext &Ctx = M.getContext();
        IntegerType *I64 = Type::getInt64Ty(Ctx);

        uint64_t FpLo = RNG.get_uint64_t();
        uint64_t FpHi = RNG.get_uint64_t();

        auto *GVLo = new GlobalVariable(M, I64, /*isConstant=*/true,
                                        GlobalValue::PrivateLinkage,
                                        ConstantInt::get(I64, FpLo),
                                        "__build_id_lo");
        auto *GVHi = new GlobalVariable(M, I64, /*isConstant=*/true,
                                        GlobalValue::PrivateLinkage,
                                        ConstantInt::get(I64, FpHi),
                                        "__build_id_hi");
        appendToUsed(M, {GVLo, GVHi});
    }

    bool runOnModule(Module &M) {
        if (!RunPolyBuild)
            return false;

        injectFingerprint(M);

        bool Changed = false;
        for (Function &F : M) {
            if (F.isDeclaration()) continue;
            Changed |= splitConstantsInFunction(F, M);
        }
        return Changed;
    }
};

} // namespace

PreservedAnalyses PolymorphicBuildPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
    PolymorphicBuild Pass;
    if (Pass.runOnModule(M))
        return PreservedAnalyses::none();
    return PreservedAnalyses::all();
}
