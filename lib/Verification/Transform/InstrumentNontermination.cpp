#include "Verification/Transform/InstrumentNontermination.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <set>
#include <vector>

using namespace llvm;

static cl::opt<bool> insertHeader(
    "instrument-nontermination-mark-header",
    cl::desc("Insert a function that marks the header of the loop"),
    cl::init(false));

namespace {

bool CloneMetadata(const Instruction *i1, Instruction *i2) {
  if (i1->hasMetadata()) {
    i2->setDebugLoc(i1->getDebugLoc());
    return true;
  }
  return false;
}

} // namespace

namespace lotus {
namespace verification {
namespace transform {

char InstrumentNonterminationPass::ID = 0;

static Function *getHeaderFun(Module *M) {
  static Function *_header = nullptr;
  if (!_header) {
    auto &Ctx = M->getContext();
    auto F = M->getOrInsertFunction("__INSTR_check_nontermination_header",
                                    Type::getVoidTy(Ctx));
    _header = cast<Function>(F.getCallee()->stripPointerCasts());
  }
  return _header;
}

static Function *getAssertFun(Module *M) {
  static Function *_assert = nullptr;
  if (!_assert) {
    auto &Ctx = M->getContext();
    auto F = M->getOrInsertFunction("__INSTR_check_nontermination",
                                    Type::getVoidTy(Ctx), Type::getInt1Ty(Ctx));
    _assert = cast<Function>(F.getCallee()->stripPointerCasts());
  }
  return _assert;
}

static Function *getFailFun(Module *M) {
  static Function *_fail = nullptr;
  if (!_fail) {
    auto &Ctx = M->getContext();
    auto F = M->getOrInsertFunction("__INSTR_fail", Type::getVoidTy(Ctx));
    _fail = cast<Function>(F.getCallee()->stripPointerCasts());
    _fail->setDoesNotReturn();
  }
  return _fail;
}

bool InstrumentNonterminationPass::runOnLoop(Loop *L, LPPassManager & /*LPM*/) {
  // For now, we detect only non-nested loops
  if (L->getParentLoop()) {
    return false;
  }

  auto *header = L->getHeader();
  assert(header);
  auto *M = header->getModule();

  // Collect variables used in the loop (allocas and globals)
  std::set<Value *> usedValues;
  for (auto *block : L->blocks()) {
    for (auto &I : *block) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        Value *ptr = LI->getPointerOperand();
        if (isa<AllocaInst>(ptr) || isa<GlobalVariable>(ptr)) {
          usedValues.insert(ptr);
        }
      } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
        Value *ptr = SI->getPointerOperand();
        if (isa<AllocaInst>(ptr) || isa<GlobalVariable>(ptr)) {
          usedValues.insert(ptr);
        }
      }
    }
  }

  if (usedValues.empty()) {
    // Empty loop - insert fail call
    Function *fail = getFailFun(M);
    for (auto I = pred_begin(header), E = pred_end(header); I != E; ++I) {
      if (L->contains(*I)) {
        auto *term = (*I)->getTerminator();
        auto *CI = CallInst::Create(fail);
        CloneMetadata(term, CI);
        CI->insertBefore(term);
      }
    }
    errs() << "Instrumented an empty loop with abort.\n";
    return true;
  }

  // Create copies of variables in the header
  std::map<Value *, Value *> mapping;
  for (auto *v : usedValues) {
    Instruction *newVal = nullptr;
    if (auto *G = dyn_cast<GlobalVariable>(v)) {
      // Create an alloca for the copy
      Function *F = header->getParent();
      newVal = new AllocaInst(G->getType()->getContainedType(0),
                              G->getType()->getAddressSpace(), nullptr, "",
                              &*F->getEntryBlock().getFirstInsertionPt());
    } else if (isa<AllocaInst>(v)) {
      // For allocas, create a new alloca
      Function *F = header->getParent();
      AllocaInst *AI = cast<AllocaInst>(v);
      newVal =
          new AllocaInst(AI->getAllocatedType(), AI->getAddressSpace(), nullptr,
                         "", &*F->getEntryBlock().getFirstInsertionPt());
    }
    if (newVal)
      mapping[v] = newVal;
  }

  if (mapping.empty())
    return false;

  // Store state at loop header
  auto *where = header->getFirstNonPHIOrDbg();
  for (auto &it : mapping) {
    auto *LI = new LoadInst(it.first->getType()->getContainedType(0), it.first,
                            "", where);
    auto *SI = new StoreInst(LI, it.second, false, LI->getAlign(), where);
    CloneMetadata(header->getTerminator(), LI);
    CloneMetadata(header->getTerminator(), SI);
  }

  if (insertHeader) {
    Function *headerFun = getHeaderFun(M);
    auto *CI = CallInst::Create(headerFun);
    CloneMetadata(header->getTerminator(), CI);
    CI->insertBefore(header->getTerminator());
  }

  // Compare old and new values after each iteration
  Function *assertFun = getAssertFun(M);
  for (auto I = pred_begin(header), E = pred_end(header); I != E; ++I) {
    if (!L->contains(*I))
      continue;

    auto *term = (*I)->getTerminator();
    Instruction *lastCond = nullptr;

    for (auto &it : mapping) {
      auto *newVal = new LoadInst(it.first->getType()->getContainedType(0),
                                  it.first, "", term);
      auto *oldVal = new LoadInst(it.second->getType()->getContainedType(0),
                                  it.second, "", term);
      auto *cmp = new ICmpInst(term, ICmpInst::ICMP_EQ, newVal, oldVal, "");

      CloneMetadata(term, newVal);
      CloneMetadata(term, oldVal);
      CloneMetadata(term, cmp);

      if (lastCond) {
        auto *And =
            BinaryOperator::Create(Instruction::And, lastCond, cmp, "", term);
        lastCond = And;
      } else {
        lastCond = cmp;
      }
    }

    if (lastCond) {
      auto *CI = CallInst::Create(assertFun, {lastCond}, "", term);
      CloneMetadata(term, CI);
    }
  }

  errs() << "Instrumented a loop with non-termination checks\n";
  return true;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<
    lotus::verification::transform::InstrumentNonterminationPass>
    X("instrument-nontermination",
      "Insert trivial checks for state space cycles");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createInstrumentNonterminationPass() {
  return new InstrumentNonterminationPass();
}

} // namespace transform
} // namespace verification
} // namespace lotus
