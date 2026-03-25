//
// Created by peiming on 11/5/19.
// Updated for modern LLVM compatibility
//
#ifndef ASER_PTA_CALLSITE_H
#define ASER_PTA_CALLSITE_H

#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>

#include "Alias/AserPTA/Util/Util.h"

namespace aser {

// wrapper around llvm::CallBase (replaces deprecated llvm::CallSite),
// but resolve constant expression evaluated to a function
class CallSite {
private:
    const llvm::CallBase* CB;
    static const llvm::Function* resolveTargetFunction(const llvm::Value*);

public:
    explicit CallSite(const llvm::Instruction* I) : CB(llvm::dyn_cast<llvm::CallBase>(I)) {}

    [[nodiscard]] inline bool isCallOrInvoke() const { 
        return CB && (llvm::isa<llvm::CallInst>(CB) || llvm::isa<llvm::InvokeInst>(CB));
    }

    [[nodiscard]] inline bool isIndirectCall() const {
        if (!CB) return false;
        if (CB->isIndirectCall()) {
            return true;
        }

        if (CB->getCalledFunction() != nullptr) {
            return false;
        }

        if (resolveTargetFunction(CB->getCalledOperand()) != nullptr) {
            return false;
        }

        auto *V = CB->getCalledOperand();
        if (auto *C = llvm::dyn_cast<llvm::Constant>(V)) {
            if (C->isNullValue()) {
                return true;
            }
        }

        return true;
    }

    [[nodiscard]] inline const llvm::Value* getCalledValue() const { 
        return CB ? CB->getCalledOperand() : nullptr;
    }

    [[nodiscard]] inline const llvm::Function* getCalledFunction() const { 
        return this->getTargetFunction();
    }

    [[nodiscard]] inline const llvm::Function* getTargetFunction() const {
        if (!CB || this->isIndirectCall()) {
            return nullptr;
        }
        auto *targetFunction = CB->getCalledFunction();
        if (targetFunction != nullptr) {
            return targetFunction;
        }

        return resolveTargetFunction(CB->getCalledOperand());
    }

    [[nodiscard]]
    inline const llvm::Value* getReturnedArgOperand() const { 
        return CB ? CB->getReturnedArgOperand() : nullptr;
    }

    [[nodiscard]]
    inline const llvm::Instruction* getInstruction() const { 
        return CB;
    }

    [[nodiscard]]
    unsigned int getNumArgOperands() const { 
        return CB ? CB->arg_size() : 0;
    }

    const llvm::Value* getArgOperand(unsigned int i) const { 
        return CB ? CB->getArgOperand(i) : nullptr;
    }

    inline auto args() const { return CB->args(); }

    [[nodiscard]]
    inline auto arg_begin() const { return CB->arg_begin(); }

    [[nodiscard]]
    inline auto arg_end() const { return CB->arg_end(); }

    inline llvm::Type* getType() const { 
        return CB ? CB->getType() : nullptr;
    }
};

}  // namespace aser

#endif
