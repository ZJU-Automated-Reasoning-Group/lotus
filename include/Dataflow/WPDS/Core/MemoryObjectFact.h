#ifndef ANALYSIS_DATAFLOW_WPDS_MEMORYOBJECTFACT_H_
#define ANALYSIS_DATAFLOW_WPDS_MEMORYOBJECTFACT_H_

#include "Dataflow/WPDS/Core/DataFlowFacts.h"

#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Value.h"

#include <map>
#include <set>
#include <vector>

namespace wpds {

/**
 * Minimal helper layer for using abstract memory objects as WPDS facts.
 *
 * The abstraction is intentionally small and field-insensitive:
 * - allocas, globals, pointer arguments, and pointer-returning calls are
 *   treated as memory objects
 * - pointer casts and GEPs collapse to their base object
 * - loads/stores can be encoded against the canonical memory object
 */
class MemoryObjectFact {
public:
  static llvm::Value *getRepresentative(llvm::Value *v) {
    if (v == nullptr) {
      return nullptr;
    }

    llvm::Value *base = v->stripPointerCasts();
    while (true) {
      if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(base)) {
        base = gep->getPointerOperand()->stripPointerCasts();
        continue;
      }
      if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(base)) {
        base = gep->getPointerOperand()->stripPointerCasts();
        continue;
      }
      break;
    }

    if (llvm::isa<llvm::AllocaInst>(base) || llvm::isa<llvm::GlobalValue>(base) ||
        llvm::isa<llvm::Argument>(base)) {
      return base;
    }

    if (auto *call = llvm::dyn_cast<llvm::CallBase>(base)) {
      if (call->getType()->isPointerTy()) {
        return call;
      }
    }

    return base;
  }

  static bool isMemoryObject(llvm::Value *v) {
    llvm::Value *rep = getRepresentative(v);
    return rep != nullptr &&
           (llvm::isa<llvm::AllocaInst>(rep) || llvm::isa<llvm::GlobalValue>(rep) ||
            llvm::isa<llvm::Argument>(rep) || llvm::isa<llvm::CallBase>(rep));
  }

  static void addRepresentativeFact(std::set<llvm::Value *> &facts,
                                    llvm::Value *v) {
    if (llvm::Value *rep = getRepresentative(v)) {
      facts.insert(rep);
    }
  }

  static void addRepresentativeFact(DataFlowFacts &facts, llvm::Value *v) {
    if (llvm::Value *rep = getRepresentative(v)) {
      facts.addFact(rep);
    }
  }

  static void addFlow(std::map<llvm::Value *, DataFlowFacts> &flow,
                      llvm::Value *src, llvm::Value *dst) {
    llvm::Value *srcRep = getRepresentative(src);
    llvm::Value *dstRep = getRepresentative(dst);
    if (srcRep == nullptr || dstRep == nullptr) {
      return;
    }
    if (!flow.count(srcRep)) {
      flow[srcRep] = DataFlowFacts::EmptySet();
    }
    flow[srcRep].addFact(dstRep);
  }

  static std::vector<llvm::Value *> pointerArgumentObjects(llvm::CallBase *call) {
    std::vector<llvm::Value *> objects;
    if (call == nullptr) {
      return objects;
    }
    for (auto &arg : call->args()) {
      if (!arg->getType()->isPointerTy()) {
        continue;
      }
      if (llvm::Value *rep = getRepresentative(arg.get())) {
        objects.push_back(rep);
      }
    }
    return objects;
  }

  static std::vector<llvm::GlobalValue *> trackedGlobals(llvm::Module &m) {
    std::vector<llvm::GlobalValue *> globals;
    for (auto &global : m.globals()) {
      globals.push_back(&global);
    }
    return globals;
  }
};

} // namespace wpds

#endif // ANALYSIS_DATAFLOW_WPDS_MEMORYOBJECTFACT_H_
