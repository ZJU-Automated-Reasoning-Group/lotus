/**
 * @file Program.cpp
 * @brief Program representation and call site resolution for AserPTA.
 *
 * Provides utilities for working with LLVM programs in the context of pointer
 * analysis, including call site target resolution and indirect call handling.
 *
 * @author peiming
 */
#include <llvm/IR/Constants.h>
#include <llvm/Support/CommandLine.h>

#include "Alias/AserPTA/PointerAnalysis/Program/CallSite.h"
#include "Alias/AserPTA/Util/Log.h"

using namespace aser;
using namespace llvm;

llvm::cl::opt<size_t> MaxIndirectTarget("max-indirect-target",
                                  llvm::cl::init(std::numeric_limits<size_t>::max()),  // by default no limitation
                                  llvm::cl::desc("max number of indirect call target that can be resolved by indirect call"));

/**
 * @brief Resolve the target function from a called value.
 *
 * Attempts to resolve the target function for direct calls by handling
 * bitcasts and global aliases. For indirect calls, this will fail and
 * trigger an assertion.
 *
 * @param calledValue The value being called (function pointer or function)
 * @return The resolved Function pointer, or nullptr if resolution fails
 */
const Function* aser::CallSite::resolveTargetFunction(const Value* calledValue) {
    if (calledValue == nullptr) {
        return nullptr;
    }

    const Value* current = calledValue;
    SmallPtrSet<const Value*, 8> visited;
    while (current != nullptr && visited.insert(current).second) {
        if (auto* function = dyn_cast<Function>(current)) {
            return function;
        }

        if (auto* globalAlias = dyn_cast<GlobalAlias>(current)) {
            const Constant* aliasee = globalAlias->getAliasee();
            current = aliasee ? aliasee->stripPointerCasts() : nullptr;
            continue;
        }

        if (auto* constExpr = dyn_cast<ConstantExpr>(current)) {
            if (constExpr->isCast() && constExpr->getNumOperands() > 0) {
                current = constExpr->getOperand(0);
                continue;
            }
        }

        if (auto* op = dyn_cast<Operator>(current)) {
            if (op->getOpcode() == Instruction::BitCast ||
                op->getOpcode() == Instruction::AddrSpaceCast) {
                current = op->getOperand(0);
                continue;
            }
        }

        const Value* stripped = current->stripPointerCasts();
        if (stripped == current) {
            break;
        }
        current = stripped;
    }

    LOG_WARN("Unable to resolve target function from calledValue: {}", *calledValue);
    return nullptr;
}
