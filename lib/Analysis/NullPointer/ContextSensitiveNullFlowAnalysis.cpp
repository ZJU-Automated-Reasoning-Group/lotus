#include "Analysis/NullPointer/ContextSensitiveNullFlowAnalysis.h"

#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckCallGraphNode.h"
#include "Analysis/NullPointer/API.h"
#include "Analysis/NullPointer/NullEquivalenceAnalysis.h"
#include "Dataflow/ControlFlow/InterCFG.h"
#include "Utils/LLVM/RecursiveTimer.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/CommandLine.h>

using namespace llvm;

namespace lotus {
namespace nullpointer {
namespace testing {

static int ContextDepthOverride = -1;

void setContextSensitiveNullContextDepthOverrideForTesting(int Depth) {
  ContextDepthOverride = Depth;
}

} // namespace testing
} // namespace nullpointer
} // namespace lotus

namespace {

static cl::opt<unsigned> CSContextDepth(
    "cs-context-depth", cl::init(3), cl::Hidden,
    cl::desc("Maximum call-string depth for context-sensitive null analysis."));
static cl::opt<unsigned> LegacyCSNFAMaxDepth(
    "csnfa-max-depth", cl::init(0), cl::Hidden,
    cl::desc("Deprecated alias for -cs-context-depth."));
static cl::opt<unsigned> LegacyCSNCAMaxDepth(
    "csnca-max-depth", cl::init(0), cl::Hidden,
    cl::desc("Deprecated alias for -cs-context-depth."));
static cl::opt<unsigned> LegacyCSNFARound(
    "csnfa-round", cl::init(0), cl::Hidden,
    cl::desc("Deprecated no-op compatibility option."));
static cl::opt<unsigned> LegacyCSNCARound(
    "csnca-round", cl::init(0), cl::Hidden,
    cl::desc("Deprecated no-op compatibility option."));
static cl::opt<bool> LegacyCSVerbose(
    "cs-verbose", cl::init(false), cl::Hidden,
    cl::desc("Deprecated no-op compatibility option."));
static cl::opt<bool> LegacyCSPrintPerFunction(
    "cs-print-per-function", cl::init(false), cl::Hidden,
    cl::desc("Deprecated no-op compatibility option."));

unsigned configuredContextDepth() {
  if (lotus::nullpointer::testing::ContextDepthOverride >= 0) {
    return static_cast<unsigned>(
        lotus::nullpointer::testing::ContextDepthOverride);
  }
  if (LegacyCSNFAMaxDepth.getNumOccurrences() > 0) {
    return LegacyCSNFAMaxDepth;
  }
  if (LegacyCSNCAMaxDepth.getNumOccurrences() > 0) {
    return LegacyCSNCAMaxDepth;
  }
  return CSContextDepth;
}

bool isGuaranteedNonNullValue(Value *V) {
  if (!V || !V->getType()->isPointerTy()) {
    return false;
  }
  V = V->stripPointerCastsAndAliases();
  if (isa<GlobalValue>(V)) {
    return true;
  }
  if (auto *Arg = dyn_cast<Argument>(V)) {
    return Arg->hasNonNullAttr();
  }
  if (auto *I = dyn_cast<Instruction>(V)) {
    if (auto *CB = dyn_cast<CallBase>(I)) {
      return CB->hasRetAttr(Attribute::NonNull);
    }
    return API::isStackAllocate(I);
  }
  return false;
}

bool isKnownMemoryIntrinsic(const CallBase *CB) {
  auto *Callee = CB ? CB->getCalledFunction() : nullptr;
  if (!Callee || !Callee->isIntrinsic()) {
    return false;
  }
  auto ID = Callee->getIntrinsicID();
  return ID >= Intrinsic::memcpy &&
         ID <= Intrinsic::memset_element_unordered_atomic;
}

struct ReturnProvenanceKey {
  Function *Callee = nullptr;
  lotus::nullpointer::CallStringContext Ctx;

  bool operator<(const ReturnProvenanceKey &Other) const {
    if (Callee != Other.Callee) {
      return Callee < Other.Callee;
    }
    return Ctx < Other.Ctx;
  }
};

struct CallerContinuation {
  CallBase *Call = nullptr;
  lotus::nullpointer::CallStringContext CallerCtx;

  bool operator<(const CallerContinuation &Other) const {
    if (Call != Other.Call) {
      return Call < Other.Call;
    }
    return CallerCtx < Other.CallerCtx;
  }
};

} // namespace

char ContextSensitiveNullFlowAnalysis::ID = 0;
static RegisterPass<ContextSensitiveNullFlowAnalysis>
    X("csnfa", "context-sensitive null value flow");

ContextSensitiveNullFlowAnalysis::ContextSensitiveNullFlowAnalysis()
    : ModulePass(ID) {}

ContextSensitiveNullFlowAnalysis::~ContextSensitiveNullFlowAnalysis() {
  if (OwnsAliasAnalysisAdapter && AAA) {
    delete AAA;
  }
}

void ContextSensitiveNullFlowAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<DyckAliasAnalysis>();
}

bool ContextSensitiveNullFlowAnalysis::runOnModule(Module &M) {
  RecursiveTimer Timer("Running Context-Sensitive NFA");

  EntryFunctions.clear();
  InFacts.clear();
  ReachableContexts.clear();
  Equivalence.clear();
  FactIds.clear();
  FactValues.clear();
  MaxContextDepth = configuredContextDepth();

  if (OwnsAliasAnalysisAdapter && AAA) {
    delete AAA;
    AAA = nullptr;
    OwnsAliasAnalysisAdapter = false;
  }

  DAA = &getAnalysis<DyckAliasAnalysis>();
  AAA = AliasAnalysisAdapter::createAdapter(&M, DAA);
  OwnsAliasAnalysisAdapter = true;

  for (auto &F : M) {
    if (!F.empty()) {
      Equivalence[&F] = std::make_unique<NullEquivalenceAnalysis>(&F);
    }
  }

  auto Canonicalize = [this](Value *V) -> Value * {
    if (!V || !V->getType()->isPointerTy()) {
      return nullptr;
    }
    V = V->stripPointerCastsAndAliases();
    if (isa<ConstantPointerNull>(V)) {
      return nullptr;
    }
    if (isa<GlobalValue>(V)) {
      return V;
    }
    if (auto *Arg = dyn_cast<Argument>(V)) {
      auto It = Equivalence.find(Arg->getParent());
      return It == Equivalence.end() ? V : It->second->get(V);
    }
    if (auto *I = dyn_cast<Instruction>(V)) {
      auto It = Equivalence.find(I->getFunction());
      return It == Equivalence.end() ? V : It->second->get(V);
    }
    return V;
  };

  auto RegisterFactValue = [this, &Canonicalize](Value *V) {
    auto *Canonical = Canonicalize(V);
    if (!Canonical) {
      return;
    }
    if (FactIds.count(Canonical)) {
      return;
    }
    FactIds[Canonical] = FactValues.size();
    FactValues.push_back(Canonical);
  };

  auto *DCG = DAA->getDyckCallGraph();
  std::set<Function *> FunctionsWithInternalCallers;
  std::set<Function *> ExternalEntryFunctions;
  if (DCG) {
    if (auto *ExternalNode = DCG->getFunction(nullptr)) {
      for (auto It = ExternalNode->child_begin(), End = ExternalNode->child_end();
           It != End; ++It) {
        if (auto *EntryFunc = (*It)->getLLVMFunction()) {
          ExternalEntryFunctions.insert(EntryFunc);
        }
      }
    }

    for (auto It = DCG->nodes_begin(), End = DCG->nodes_end(); It != End; ++It) {
      auto *CallerNode = *It;
      if (!CallerNode) {
        continue;
      }
      auto *Caller = CallerNode->getLLVMFunction();
      if (!Caller) {
        continue;
      }
      for (auto EdgeIt = CallerNode->child_edge_begin(),
                EdgeEnd = CallerNode->child_edge_end();
           EdgeIt != EdgeEnd; ++EdgeIt) {
        auto *CalleeNode = EdgeIt->second;
        if (!CalleeNode) {
          continue;
        }
        if (auto *Callee = CalleeNode->getLLVMFunction()) {
          FunctionsWithInternalCallers.insert(Callee);
        }
      }
    }
  }

  for (auto &F : M) {
    if (!F.empty() && !FunctionsWithInternalCallers.count(&F)) {
      EntryFunctions.insert(&F);
    }
  }
  if (EntryFunctions.empty()) {
    for (auto &F : M) {
      if (!F.empty() && ExternalEntryFunctions.count(&F)) {
        EntryFunctions.insert(&F);
      }
    }
  }
  if (EntryFunctions.empty()) {
    for (auto &F : M) {
      if (!F.empty()) {
        EntryFunctions.insert(&F);
      }
    }
  }

  for (auto &G : M.globals()) {
    if (G.getType()->isPointerTy()) {
      RegisterFactValue(&G);
    }
  }

  for (auto &F : M) {
    if (F.empty()) {
      continue;
    }
    for (auto &Arg : F.args()) {
      if (Arg.getType()->isPointerTy()) {
        RegisterFactValue(&Arg);
      }
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (I.getType()->isPointerTy()) {
          RegisterFactValue(&I);
        }
        for (auto &Use : I.operands()) {
          if (Use->getType()->isPointerTy()) {
            RegisterFactValue(Use.get());
          }
        }
      }
    }
  }

  dataflow::controlflow::LLVMInterCFG ICFG(
      &M, [this](Instruction *Inst) -> std::vector<Function *> {
        std::vector<Function *> Callees;
        auto *Call = dyn_cast<CallBase>(Inst);
        if (!Call) {
          return Callees;
        }

        auto AddCallee = [&Callees](Function *F) {
          if (!F) {
            return;
          }
          if (std::find(Callees.begin(), Callees.end(), F) == Callees.end()) {
            Callees.push_back(F);
          }
        };

        if (DAA) {
          auto *Graph = DAA->getDyckCallGraph();
          auto *CallerNode = Graph ? Graph->getFunction(Call->getFunction()) : nullptr;
          auto *Resolved = CallerNode ? CallerNode->getCall(Call) : nullptr;
          if (auto *CC = dyn_cast_or_null<CommonCall>(Resolved)) {
            AddCallee(CC->getCalledFunction());
          } else if (auto *PC = dyn_cast_or_null<PointerCall>(Resolved)) {
            for (auto *Callee : *PC) {
              AddCallee(Callee);
            }
          }
        }

        if (Callees.empty()) {
          AddCallee(Call->getCalledFunction());
        }
        return Callees;
      });

  auto EmptyFacts = [this]() { return BitVector(FactValues.size()); };

  auto SetFact = [this, &Canonicalize](BitVector &Facts, Value *V) {
    auto *Canonical = Canonicalize(V);
    if (!Canonical) {
      return;
    }
    auto It = FactIds.find(Canonical);
    if (It != FactIds.end() && It->second < Facts.size()) {
      Facts.set(It->second);
    }
  };

  auto TestFact = [this, &Canonicalize](const BitVector &Facts, Value *V) {
    if (isGuaranteedNonNullValue(V)) {
      return true;
    }
    auto *Canonical = Canonicalize(V);
    if (!Canonical) {
      return false;
    }
    auto It = FactIds.find(Canonical);
    return It != FactIds.end() && It->second < Facts.size() &&
           Facts.test(It->second);
  };

  auto FilterGlobals = [this](const BitVector &Facts) {
    BitVector Filtered(FactValues.size());
    for (unsigned I = 0; I < Facts.size(); ++I) {
      if (!Facts.test(I)) {
        continue;
      }
      if (isa<GlobalValue>(FactValues[I])) {
        Filtered.set(I);
      }
    }
    return Filtered;
  };

  auto ApplyLocalTransfer =
      [&, this](Instruction *Inst, const Context &Ctx,
                const BitVector &In) -> BitVector {
    (void)Ctx;
    BitVector Out = In;
    if (!Inst) {
      return Out;
    }

    switch (Inst->getOpcode()) {
    case Instruction::Load:
      SetFact(Out, cast<LoadInst>(Inst)->getPointerOperand());
      break;
    case Instruction::Store:
      SetFact(Out, cast<StoreInst>(Inst)->getPointerOperand());
      break;
    case Instruction::GetElementPtr:
      SetFact(Out, cast<GetElementPtrInst>(Inst)->getPointerOperand());
      break;
    case Instruction::Alloca:
      SetFact(Out, Inst);
      break;
    case Instruction::AddrSpaceCast:
    case Instruction::BitCast:
      if (TestFact(In, Inst->getOperand(0))) {
        SetFact(Out, Inst);
      }
      break;
    case Instruction::PHI:
    case Instruction::Select:
      if (Inst->getType()->isPointerTy()) {
        bool AllNonNull = true;
        for (Value *Op : Inst->operands()) {
          if (!Op->getType()->isPointerTy()) {
            continue;
          }
          if (!TestFact(In, Op)) {
            AllNonNull = false;
            break;
          }
        }
        if (AllNonNull) {
          SetFact(Out, Inst);
        }
      }
      break;
    case Instruction::Call:
    case Instruction::Invoke:
    case Instruction::CallBr: {
      auto *CB = cast<CallBase>(Inst);
      if (CB->getType()->isPointerTy() && CB->hasRetAttr(Attribute::NonNull)) {
        SetFact(Out, CB);
      }
      if (isKnownMemoryIntrinsic(CB)) {
        for (Value *Arg : CB->args()) {
          if (Arg->getType()->isPointerTy()) {
            SetFact(Out, Arg);
          }
        }
      }
      if (!CB->getCalledFunction() &&
          CB->getCalledOperand()->getType()->isPointerTy()) {
        SetFact(Out, CB->getCalledOperand());
      }
      break;
    }
    default:
      break;
    }
    return Out;
  };

  auto ApplyNormalEdgeTransfer = [&, this](Instruction *Inst, Instruction *Succ,
                                           BitVector Facts) {
    auto *Br = dyn_cast<BranchInst>(Inst);
    if (!Br || !Br->isConditional()) {
      return Facts;
    }

    auto *Cmp = dyn_cast<ICmpInst>(Br->getCondition());
    if (!Cmp || !Cmp->isEquality()) {
      return Facts;
    }
    auto *Op0 = Cmp->getOperand(0);
    auto *Op1 = Cmp->getOperand(1);
    if (!Op0->getType()->isPointerTy() || !Op1->getType()->isPointerTy()) {
      return Facts;
    }

    auto *NullOp = dyn_cast<ConstantPointerNull>(Op0);
    Value *Candidate = Op1;
    if (!NullOp) {
      NullOp = dyn_cast<ConstantPointerNull>(Op1);
      Candidate = Op0;
    }
    if (!NullOp) {
      return Facts;
    }

    int SuccIndex = -1;
    for (unsigned I = 0; I < Br->getNumSuccessors(); ++I) {
      if (&Br->getSuccessor(I)->front() == Succ) {
        SuccIndex = static_cast<int>(I);
        break;
      }
    }
    if (SuccIndex < 0) {
      return Facts;
    }

    bool NonNullBranch =
        (Cmp->getPredicate() == ICmpInst::ICMP_EQ && SuccIndex == 1) ||
        (Cmp->getPredicate() == ICmpInst::ICMP_NE && SuccIndex == 0);
    if (NonNullBranch) {
      SetFact(Facts, Candidate);
    }
    return Facts;
  };

  std::deque<ContextKey> Queue;
  std::set<ContextKey> InQueue;
  std::map<ReturnProvenanceKey, std::set<CallerContinuation>> ReturnProvenance;

  auto MergeState = [&](const ContextKey &Key, const BitVector &Incoming) {
    auto It = InFacts.find(Key);
    bool Changed = false;
    if (It == InFacts.end()) {
      InFacts.emplace(Key, Incoming);
      Changed = true;
    } else {
      BitVector Merged = It->second;
      Merged &= Incoming;
      if (Merged != It->second) {
        It->second = Merged;
        Changed = true;
      }
    }

    ReachableContexts[Key.Inst].insert(Key.Ctx);
    if (Changed && !InQueue.count(Key)) {
      Queue.push_back(Key);
      InQueue.insert(Key);
    }
  };

  auto RecordReturnProvenance = [&](Function *Callee, const Context &CalleeCtx,
                                    CallBase *Call,
                                    const Context &CallerCtx) -> bool {
    if (!Callee || !Call) {
      return false;
    }

    auto &Continuations = ReturnProvenance[{Callee, CalleeCtx}];
    bool Inserted =
        Continuations.insert(CallerContinuation{Call, CallerCtx}).second;
    if (!Inserted) {
      return false;
    }

    for (auto *Exit : ICFG.getExitPointsOf(Callee)) {
      if (!Exit) {
        continue;
      }

      auto ReachableIt = ReachableContexts.find(Exit);
      if (ReachableIt == ReachableContexts.end() ||
          !ReachableIt->second.count(CalleeCtx)) {
        continue;
      }

      ContextKey ExitKey{Exit, CalleeCtx};
      if (!InQueue.count(ExitKey)) {
        Queue.push_back(ExitKey);
        InQueue.insert(ExitKey);
      }
    }

    return true;
  };

  for (auto *Entry : EntryFunctions) {
    auto Starts = ICFG.getStartPointsOf(Entry);
    if (Starts.empty()) {
      continue;
    }
    BitVector Seed = EmptyFacts();
    for (auto &Arg : Entry->args()) {
      if (Arg.getType()->isPointerTy() && Arg.hasNonNullAttr()) {
        SetFact(Seed, &Arg);
      }
    }
    MergeState({Starts.front(), Context()}, Seed);
  }

  while (!Queue.empty()) {
    ContextKey Key = Queue.front();
    Queue.pop_front();
    InQueue.erase(Key);

    auto FactsIt = InFacts.find(Key);
    if (FactsIt == InFacts.end()) {
      continue;
    }

    const BitVector In = FactsIt->second;
    BitVector LocalOut = ApplyLocalTransfer(Key.Inst, Key.Ctx, In);
    auto PropagateReturnToCall = [&](ReturnInst *Ret, CallBase *Call,
                                     const Context &CallerCtx) {
      if (!Call) {
        return;
      }

      auto CallIt = InFacts.find({Call, CallerCtx});
      if (CallIt == InFacts.end()) {
        return;
      }

      BitVector ReturnFacts =
          ApplyLocalTransfer(Call, CallerCtx, CallIt->second);
      if (auto *RetVal = Ret->getReturnValue()) {
        if (Call->getType()->isPointerTy() && TestFact(In, RetVal)) {
          SetFact(ReturnFacts, Call);
        }
      }

      for (auto *RetSite : ICFG.getReturnSitesOfCallAt(Call)) {
        MergeState({RetSite, CallerCtx}, ReturnFacts);
      }
    };

    if (auto *CB = dyn_cast<CallBase>(Key.Inst)) {
      auto Callees = ICFG.getCalleesOfCallAt(CB);
      bool HasDefinedCallee = false;
      bool HasExternalPath = Callees.empty();

      for (auto *Callee : Callees) {
        if (!Callee || Callee->isDeclaration() || Callee->empty()) {
          HasExternalPath = true;
          continue;
        }

        HasDefinedCallee = true;
        Context CalleeCtx = Key.Ctx;
        CalleeCtx.append(CB, MaxContextDepth);
        RecordReturnProvenance(Callee, CalleeCtx, CB, Key.Ctx);
        BitVector CalleeFacts = FilterGlobals(LocalOut);
        for (unsigned I = 0; I < CB->arg_size() && I < Callee->arg_size(); ++I) {
          auto *Actual = CB->getArgOperand(I);
          auto *Formal = Callee->getArg(I);
          if (!Formal->getType()->isPointerTy()) {
            continue;
          }
          if (Formal->hasNonNullAttr() || TestFact(LocalOut, Actual)) {
            SetFact(CalleeFacts, Formal);
          }
        }

        for (auto *Start : ICFG.getStartPointsOf(Callee)) {
          MergeState({Start, CalleeCtx}, CalleeFacts);
        }
      }

      if (!HasDefinedCallee || HasExternalPath) {
        for (auto *RetSite : ICFG.getReturnSitesOfCallAt(CB)) {
          MergeState({RetSite, Key.Ctx}, LocalOut);
        }
      }
      continue;
    }

    if (auto *Ret = dyn_cast<ReturnInst>(Key.Inst)) {
      auto ProvIt = ReturnProvenance.find({Ret->getFunction(), Key.Ctx});
      if (ProvIt != ReturnProvenance.end()) {
        for (const auto &Continuation : ProvIt->second) {
          PropagateReturnToCall(Ret, Continuation.Call,
                                Continuation.CallerCtx);
        }
      } else if (Key.Ctx.empty()) {
        // Empty-context returns without recorded caller provenance correspond
        // to top-level or externally initiated execution.
        for (auto *CallerInst : ICFG.getCallersOf(Ret->getFunction())) {
          auto *Caller = dyn_cast<CallBase>(CallerInst);
          if (Caller == nullptr) {
            continue;
          }
          PropagateReturnToCall(Ret, Caller, Context());
        }
      }
      continue;
    }

    for (auto *Succ : ICFG.getSuccsOf(Key.Inst,
                                      dataflow::controlflow::FlowDirection::Forward)) {
      MergeState({Succ, Key.Ctx}, ApplyNormalEdgeTransfer(Key.Inst, Succ, LocalOut));
    }
  }

  return false;
}

bool ContextSensitiveNullFlowAnalysis::notNull(Value *Ptr, Instruction *Inst,
                                               const Context &Ctx) const {
  if (!Ptr || !Inst || !Ptr->getType()->isPointerTy()) {
    return false;
  }
  if (isGuaranteedNonNullValue(Ptr)) {
    return true;
  }

  auto Canonicalize = [this](Value *V) -> Value * {
    if (!V || !V->getType()->isPointerTy()) {
      return nullptr;
    }
    V = V->stripPointerCastsAndAliases();
    if (isa<ConstantPointerNull>(V)) {
      return nullptr;
    }
    if (isa<GlobalValue>(V)) {
      return V;
    }
    if (auto *Arg = dyn_cast<Argument>(V)) {
      auto It = Equivalence.find(Arg->getParent());
      return It == Equivalence.end() ? V : It->second->get(V);
    }
    if (auto *I = dyn_cast<Instruction>(V)) {
      auto It = Equivalence.find(I->getFunction());
      return It == Equivalence.end() ? V : It->second->get(V);
    }
    return V;
  };

  auto It = InFacts.find({Inst, Ctx});
  if (It == InFacts.end()) {
    return false;
  }

  auto *Canonical = Canonicalize(Ptr);
  if (!Canonical) {
    return false;
  }
  auto IdIt = FactIds.find(Canonical);
  return IdIt != FactIds.end() && IdIt->second < It->second.size() &&
         It->second.test(IdIt->second);
}

std::vector<ContextSensitiveNullFlowAnalysis::Context>
ContextSensitiveNullFlowAnalysis::getReachableContexts(Instruction *Inst) const {
  std::vector<Context> Result;
  auto It = ReachableContexts.find(Inst);
  if (It == ReachableContexts.end()) {
    return Result;
  }
  Result.insert(Result.end(), It->second.begin(), It->second.end());
  return Result;
}

std::string
ContextSensitiveNullFlowAnalysis::getContextString(const Context &Ctx) const {
  return Ctx.str();
}

bool ContextSensitiveNullFlowAnalysis::isEntryFunction(const Function *F) const {
  return F && EntryFunctions.count(const_cast<Function *>(F));
}
