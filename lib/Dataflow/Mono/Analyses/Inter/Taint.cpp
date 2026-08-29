#include "Dataflow/Mono/Analyses/Inter/Taint.h"

#include "llvm/IR/Instructions.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Dataflow/Mono/Domains/TaintDomain.h"
#include "Dataflow/Mono/Solver/InterSolver.h"

#include <algorithm>
#include <memory>

using namespace llvm;

namespace mono {
namespace {

std::vector<Function *>
resolveIndirectCalleesWithAA(Instruction *CallSite,
                             lotus::AliasAnalysisWrapper *AA) {
  std::vector<Function *> Callees;
  auto *Call = dyn_cast_or_null<CallBase>(CallSite);
  if (Call == nullptr || AA == nullptr || !AA->isInitialized()) {
    return Callees;
  }

  std::vector<const Function *> Targets;
  AA->getIndirectCallTargets(Call, Targets);
  for (const auto *Target : Targets) {
    if (Target == nullptr) {
      continue;
    }
    auto *MutableTarget = const_cast<Function *>(Target);
    if (std::find(Callees.begin(), Callees.end(), MutableTarget) ==
        Callees.end()) {
      Callees.push_back(MutableTarget);
    }
  }
  return Callees;
}

class InterMonoTaintProblem : public InterMonoProblem<TaintAnalysisTypes> {
public:
  using mono_container_t = typename TaintAnalysisTypes::mono_container_t;

  InterMonoTaintProblem(Function *Entry, const InterMonoTaintConfig &Config,
                        lotus::AliasAnalysisWrapper *AA)
      : InterMonoProblem<TaintAnalysisTypes>({Entry}, AA), Config(Config), AA(AA) {}

  mono_container_t normalFlow(Instruction *Inst,
                              const mono_container_t &In) override {
    if (auto *Call = dyn_cast<CallBase>(Inst)) {
      return applyCallSite(Call, resolveCallTargets(Call), In);
    }
    return applyInstructionFlow(Inst, In);
  }

  mono_container_t callFlow(Instruction *CallSite, Function *Callee,
                            const mono_container_t &In) override {
    mono_container_t Out;
    if (Callee == nullptr) {
      return Out;
    }

    auto *Call = dyn_cast<CallBase>(CallSite);
    if (Call == nullptr) {
      return Out;
    }

    auto *ArgIt = Callee->arg_begin();
    for (auto &Arg : Call->args()) {
      if (ArgIt == Callee->arg_end()) {
        break;
      }
      if (In.count(Arg.get())) {
        Out.insert(&*ArgIt);
      }
      ++ArgIt;
    }

    for (auto *Val : In) {
      if (isa<GlobalValue>(Val)) {
        Out.insert(Val);
      }
    }

    return Out;
  }

  mono_container_t returnFlow(Instruction *CallSite, Function *Callee,
                              Instruction *ExitStmt, Instruction *RetSite,
                              const mono_container_t &In) override {
    (void)RetSite;

    mono_container_t Out;
    for (auto *Val : In) {
      if (isa<GlobalValue>(Val)) {
        Out.insert(Val);
      }
    }

    auto *Ret = dyn_cast<ReturnInst>(ExitStmt);
    if (Ret != nullptr && CallSite != nullptr) {
      if (auto *RetVal = Ret->getReturnValue()) {
        if (In.count(RetVal) && !CallSite->getType()->isVoidTy()) {
          Out.insert(CallSite);
        }
      }
    }

    auto *Call = dyn_cast_or_null<CallBase>(CallSite);
    if (Call != nullptr && Callee != nullptr) {
      unsigned Index = 0;
      for (auto &Arg : Callee->args()) {
        if (Index >= Call->arg_size()) {
          break;
        }
        if (Arg.getType()->isPointerTy() && In.count(&Arg)) {
          taintAliases(Call->getArgOperand(Index), Out);
        }
        ++Index;
      }
    }

    return Out;
  }

  mono_container_t callToRetFlow(Instruction *CallSite, Instruction *RetSite,
                                 ArrayRef<Function *> Callees,
                                 const mono_container_t &In) override {
    (void)CallSite;
    (void)RetSite;
    (void)Callees;

    // Call-specific gen/kill effects are modeled in normalFlow() so the
    // call-to-return edge only needs to forward the already computed OUT set.
    return In;
  }

  std::unordered_map<Instruction *, mono_container_t> initialSeeds() override {
    std::unordered_map<Instruction *, mono_container_t> Seeds;
    auto *F = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (F == nullptr || F->empty()) {
      return Seeds;
    }
    auto &EntryFacts = Seeds[&F->getEntryBlock().front()];
    if (Config.SeedEntryArguments) {
      for (auto &Arg : F->args()) {
        EntryFacts.insert(&Arg);
      }
    }
    return Seeds;
  }

  std::vector<Function *>
  resolve_indirect_callees(Instruction *CallSite) const override {
    auto Callees = resolveIndirectCalleesWithAA(CallSite, AA);
    if (!Callees.empty()) {
      return Callees;
    }
    return InterMonoProblem<TaintAnalysisTypes>::resolve_indirect_callees(CallSite);
  }

  const InterMonoTaintReport &getReport() const { return Report; }

private:
  struct AliasPartition {
    std::vector<Value *> MustAliases;
    std::vector<Value *> MayAliases;
  };

  AliasPartition classifyAliases(const Value *Ptr,
                                 const mono_container_t &Facts) const {
    AliasPartition AP;
    if (Ptr == nullptr) {
      return AP;
    }
    const bool HaveAA = AA != nullptr && AA->isInitialized();
    const Value *PtrBase = Ptr->stripPointerCasts();

    auto Add = [&](Value *Candidate) {
      if (Candidate == nullptr || !Candidate->getType()->isPointerTy()) {
        return;
      }
      const Value *CandidateBase = Candidate->stripPointerCasts();
      if (PtrBase != nullptr && CandidateBase != nullptr &&
          PtrBase == CandidateBase) {
        AP.MustAliases.push_back(Candidate);
        return;
      }
      if (Candidate == Ptr) {
        AP.MustAliases.push_back(Candidate);
        return;
      }
      if (!HaveAA || !Ptr->getType()->isPointerTy()) {
        AP.MayAliases.push_back(Candidate);
        return;
      }
      auto Res = AA->query(Ptr, Candidate);
      if (Res == AliasResult::MustAlias) {
        AP.MustAliases.push_back(Candidate);
      } else if (Res != AliasResult::NoAlias) {
        AP.MayAliases.push_back(Candidate);
      }
    };

    Add(const_cast<Value *>(Ptr));
    for (auto *V : Facts) {
      Add(V);
    }
    return AP;
  }

  bool isTaintedValue(const Value *V, const mono_container_t &In) const {
    if (V == nullptr) {
      return false;
    }
    if (In.count(const_cast<Value *>(V))) {
      return true;
    }
    if (!V->getType()->isPointerTy()) {
      return false;
    }
    const bool HaveAA = AA != nullptr && AA->isInitialized();
    const Value *VBase = V->stripPointerCasts();

    for (auto *Candidate : In) {
      if (Candidate == nullptr || !Candidate->getType()->isPointerTy()) {
        continue;
      }
      const Value *CandidateBase = Candidate->stripPointerCasts();
      if (VBase != nullptr && CandidateBase != nullptr &&
          VBase == CandidateBase) {
        return true;
      }
      if (!HaveAA) {
        return true;
      }
      if (AA->query(V, Candidate) != AliasResult::NoAlias) {
        return true;
      }
    }
    return false;
  }

  void taintAliases(Value *Ptr, mono_container_t &Out) const {
    if (Ptr == nullptr) {
      return;
    }
    auto AP = classifyAliases(Ptr, Out);
    for (auto *Alias : AP.MustAliases) {
      Out.insert(Alias);
    }
    for (auto *Alias : AP.MayAliases) {
      Out.insert(Alias);
    }
  }

  void untaintMustAliases(Value *Ptr, mono_container_t &Out) const {
    if (Ptr == nullptr) {
      return;
    }
    auto AP = classifyAliases(Ptr, Out);
    for (auto *Alias : AP.MustAliases) {
      Out.erase(Alias);
    }
  }

  bool isSourceFunction(const Function *F) const {
    if (F == nullptr) {
      return false;
    }
    return Config.SourceFunctions.count(F->getName().str()) > 0;
  }

  bool isSinkFunction(const Function *F) const {
    if (F == nullptr) {
      return false;
    }
    return Config.SinkFunctions.count(F->getName().str()) > 0;
  }

  bool isSanitizerFunction(const Function *F) const {
    if (F == nullptr) {
      return false;
    }
    return Config.SanitizerFunctions.count(F->getName().str()) > 0;
  }

  std::vector<Function *> resolveCallTargets(CallBase *Call) const {
    if (Call == nullptr) {
      return {};
    }
    if (auto *Direct = Call->getCalledFunction()) {
      return {Direct};
    }
    return resolve_indirect_callees(Call);
  }

  void recordSinkLeak(CallBase *Call, ArrayRef<Function *> Callees,
                      const mono_container_t &In) {
    if (Call == nullptr ||
        !std::any_of(Callees.begin(), Callees.end(),
                     [this](const Function *Callee) {
                       return isSinkFunction(Callee);
                     })) {
      return;
    }
    for (auto &Arg : Call->args()) {
      if (isTaintedValue(Arg.get(), In)) {
        Report.Leaks[Call].insert(Arg.get());
      }
    }
  }

  mono_container_t applyInstructionFlow(Instruction *Inst,
                                        const mono_container_t &In) {
    mono_container_t Out = In;

    if (auto *Store = dyn_cast<StoreInst>(Inst)) {
      if (isTaintedValue(Store->getValueOperand(), In)) {
        taintAliases(Store->getPointerOperand(), Out);
      } else if (isTaintedValue(Store->getPointerOperand(), In)) {
        // Strong update: clear only must-aliases. May-aliases are weak updates.
        untaintMustAliases(Store->getPointerOperand(), Out);
      }
      return Out;
    }

    if (auto *Load = dyn_cast<LoadInst>(Inst)) {
      if (isTaintedValue(Load->getPointerOperand(), In)) {
        Out.insert(Load);
      }
      return Out;
    }

    if (!Inst->getType()->isVoidTy()) {
      for (auto &Op : Inst->operands()) {
        if (isTaintedValue(Op.get(), In)) {
          Out.insert(Inst);
          break;
        }
      }
    }

    return Out;
  }

  void sanitizeCallResult(CallBase *Call, mono_container_t &Out) const {
    if (Call == nullptr || Call->getType()->isVoidTy()) {
      return;
    }
    Out.erase(Call);
  }

  mono_container_t applyCallSite(CallBase *Call, ArrayRef<Function *> Callees,
                                 const mono_container_t &In) {
    mono_container_t Out = applyInstructionFlow(Call, In);

    recordSinkLeak(Call, Callees, In);

    if (std::any_of(Callees.begin(), Callees.end(),
                    [this](const Function *Callee) {
                      return isSourceFunction(Callee);
                    })) {
      if (!Call->getType()->isVoidTy()) {
        Out.insert(Call);
      }
      if (Config.TaintPointerArgsFromSources) {
        for (auto &Arg : Call->args()) {
          if (Arg->getType()->isPointerTy()) {
            taintAliases(Arg.get(), Out);
          }
        }
      }
    }

    if (!Callees.empty() &&
        std::all_of(Callees.begin(), Callees.end(),
                    [this](const Function *Callee) {
                      return isSanitizerFunction(Callee);
                    })) {
      sanitizeCallResult(Call, Out);
    }

    return Out;
  }

  const InterMonoTaintConfig &Config;
  InterMonoTaintReport Report;
  lotus::AliasAnalysisWrapper *AA;
};

} // namespace

InterMonoTaintAnalysisResult
runInterMonoTaintAnalysis(Function *Entry, const InterMonoTaintConfig &Config) {
  InterMonoTaintAnalysisResult Result;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Result;
  }

  auto AA = std::make_unique<lotus::AliasAnalysisWrapper>(
      *Entry->getParent(),
      lotus::AAConfig(lotus::AAConfig::Implementation::DyckAA,
                      lotus::AAConfig::ContextSensitivity::None, 0, true,
                      lotus::AAConfig::Solver::Default));
  InterMonoTaintProblem Problem(Entry, Config, AA.get());
  InterMonoSolver<TaintAnalysisTypes, kDefaultTaintCallStringLength> Solver(Problem);
  Solver.solve();

  if (auto *Raw = Solver.getResults()) {
    Result.Results = std::make_unique<InterMonoTaintResult>(*Raw);
  }
  Result.Report = Problem.getReport();
  return Result;
}

} // namespace mono
