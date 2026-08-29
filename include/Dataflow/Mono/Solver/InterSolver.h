#ifndef LOTUS_DATAFLOW_MONO_SOLVER_INTERSOLVER_H_
#define LOTUS_DATAFLOW_MONO_SOLVER_INTERSOLVER_H_

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/ControlFlow/InterCFG.h"
#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Solver/CallStringSolver.h"
#include "Utils/LLVM/CallUtils.h"
#include "Utils/LLVM/FunctionUtils.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace mono {

template <typename AnalysisTypesT, unsigned K> class InterMonoSolver {
public:
  using ProblemTy = InterMonoProblem<AnalysisTypesT>;
  using mono_container_t = typename AnalysisTypesT::mono_container_t;
  using ResultTy =
      dataflow::ContextSensitiveDataFlowResult<K, mono_container_t>;
  using Context = typename ResultTy::Context;
  using ContextKey = typename ResultTy::ContextKey;
  using ICFG = dataflow::controlflow::InterCFG;

  explicit InterMonoSolver(ProblemTy &Problem) : Problem(Problem) {}

  void solve() {
    auto &Entries = Problem.getEntryPoints();
    const auto Seeds = Problem.initialSeeds();
    if (Entries.empty() && Seeds.empty()) {
      Result.reset();
      return;
    }

    dataflow::CallStringInterProceduralDataFlowEngine<K, mono_container_t>
        Engine;

    auto ComputeGEN = [this](llvm::Instruction *Inst, ResultTy *DF) {
      computeGEN(Inst, DF);
    };
    auto ComputeKILL = [this](llvm::Instruction *Inst, ResultTy *DF) {
      computeKILL(Inst, DF);
    };
    auto InitializeIN = [this](llvm::Instruction *Inst, mono_container_t &IN) {
      initializeIN(Inst, IN);
    };
    auto InitializeOUT = [this](llvm::Instruction *Inst,
                                mono_container_t &OUT) {
      initializeOUT(Inst, OUT);
    };
    auto ComputeIN = [this](llvm::Instruction *Inst,
                            llvm::Instruction *PredInst, const Context &PredCtx,
                            const Context &CurrentCtx, mono_container_t &IN,
                            ResultTy *DF) {
      computeIN(Inst, PredInst, PredCtx, CurrentCtx, IN, DF);
    };
    auto ComputeOUT = [this](llvm::Instruction *Inst, const Context &Ctx,
                             mono_container_t &OUT,
                             ResultTy *DF) { computeOUT(Inst, Ctx, OUT, DF); };
    auto Equal = [this](const mono_container_t &Lhs,
                        const mono_container_t &Rhs) {
      return Problem.equal(Lhs, Rhs);
    };

    std::vector<ContextKey> RootKeys;
    std::map<ContextKey, mono_container_t> SeedIns;

    Context EmptyCtx;
    for (const auto &Seed : Seeds) {
      RootKeys.push_back(ContextKey{Seed.first, EmptyCtx});
      SeedIns[ContextKey{Seed.first, EmptyCtx}] = Seed.second;
    }

    if (RootKeys.empty()) {
      for (auto *Entry : Entries) {
        if (Entry == nullptr || Entry->isDeclaration() || Entry->empty()) {
          continue;
        }
        if (Problem.direction() ==
            ::dataflow::controlflow::FlowDirection::Backward) {
          for (auto *Exit :
               Entry->getBasicBlockList().back().getTerminator()
                   ? std::vector<llvm::Instruction *>{Entry->getBasicBlockList()
                                                          .back()
                                                          .getTerminator()}
                   : std::vector<llvm::Instruction *>{}) {
            if (Exit != nullptr) {
              RootKeys.push_back(ContextKey{Exit, EmptyCtx});
            }
          }
          for (auto &BB : *Entry) {
            if (auto *Ret =
                    llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
              RootKeys.push_back(ContextKey{Ret, EmptyCtx});
            }
          }
        } else {
          RootKeys.push_back(
              ContextKey{&*Entry->getEntryBlock().begin(), EmptyCtx});
        }
      }
    }

    llvm::Module *M = nullptr;
    if (!RootKeys.empty() && RootKeys.front().Inst != nullptr) {
      M = RootKeys.front().Inst->getModule();
    }

    // Select ICFG (provided by the problem or LLVM fallback).
    OwnedICF.reset();
    ICF = nullptr;
    if (auto *Provided = Problem.getICFG()) {
      ICF = Provided;
    } else {
      auto GetCallees = [this](llvm::Instruction *Inst) {
        return Problem.getCalleesOfCallAt(Inst);
      };
      OwnedICF = std::make_unique<::dataflow::controlflow::LLVMInterCFG>(
          M, GetCallees);
      ICF = OwnedICF.get();
    }

    if (Problem.direction() ==
        ::dataflow::controlflow::FlowDirection::Backward) {
      Result = Engine.applyBackwardFromSeeds(
          M, RootKeys, ICF, SeedIns, ComputeGEN, ComputeKILL, InitializeIN,
          InitializeOUT, ComputeIN, ComputeOUT, Equal);
    } else {
      Result = Engine.applyForwardFromSeeds(
          M, RootKeys, ICF, SeedIns, ComputeGEN, ComputeKILL, InitializeIN,
          InitializeOUT, ComputeIN, ComputeOUT, Equal);
    }

    if (Result) {
      Result->setMissingFactFallback(Problem.bottom());
    }
  }

  const ResultTy *getResults() const { return Result.get(); }

  /// Returns the IN facts at \p Stmt merged across all call-string contexts.
  /// This is a convenience query; use the raw IN/OUT maps below when checking
  /// context-sensitive behaviour because they preserve the per-context split.
  /// Uses the problem's join to combine per-context facts. Returns
  /// `Problem.bottom()` if no results or \p Stmt has no entries.
  mono_container_t getResultsAt(llvm::Instruction *Stmt) const {
    if (!Result) {
      return Problem.bottom();
    }
    mono_container_t merged;
    bool first = true;
    for (const auto &Cell : Result->getINMap()) {
      if (Cell.first.Inst != Stmt) {
        continue;
      }
      if (first) {
        merged = Cell.second;
        first = false;
      } else {
        merged = Problem.join(merged, Cell.second);
      }
    }
    if (first) {
      return Problem.bottom();
    }
    return merged;
  }

  /// Raw IN map: (Instruction, Context) -> facts. Null if solve() not run or
  /// produced no results.
  const std::map<ContextKey, mono_container_t> *getAnalysisINMap() const {
    return Result ? &Result->getINMap() : nullptr;
  }

  /// Raw OUT map: (Instruction, Context) -> facts. Null if solve() not run or
  /// produced no results.
  const std::map<ContextKey, mono_container_t> *getAnalysisOUTMap() const {
    return Result ? &Result->getOUTMap() : nullptr;
  }

  void dumpResults(llvm::raw_ostream &OS = llvm::outs()) const {
    OS << "\n================ InterMonoSolver results ================\n";
    if (!Result) {
      OS << "No results computed!\n";
      return;
    }
    for (const auto &Cell : Result->getINMap()) {
      const auto &Key = Cell.first;
      const auto &Facts = Cell.second;
      OS << "Instruction: ";
      if (Key.Inst != nullptr) {
        OS << *Key.Inst;
      } else {
        OS << "<null>";
      }
      OS << "\n";
      Key.Ctx.print(OS) << "\n";
      OS << "Facts: ";
      if (Facts.empty()) {
        OS << "EMPTY\n";
      } else {
        Problem.printContainer(OS, Facts);
        OS << "\n";
      }
    }
  }

  void emitTextReport(llvm::raw_ostream & /*OS*/ = llvm::outs()) const {}
  void emitGraphicalReport(llvm::raw_ostream & /*OS*/ = llvm::outs()) const {}

private:
  static bool isContinuationOfCall(llvm::Instruction *Inst,
                                   llvm::Instruction *CallInst) {
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallInst);
    for (auto *Cont : lotus::llvm_utils::getNormalCallContinuations(Call)) {
      if (Cont == Inst) {
        return true;
      }
    }
    return false;
  }

  void initializeIN(llvm::Instruction *, mono_container_t &IN) {
    IN = Problem.bottom();
  }

  void initializeOUT(llvm::Instruction *, mono_container_t &OUT) {
    OUT = Problem.bottom();
  }

  void computeGEN(llvm::Instruction * /*Inst*/, ResultTy * /*DF*/) {}
  void computeKILL(llvm::Instruction * /*Inst*/, ResultTy * /*DF*/) {}

  void computeIN(llvm::Instruction *Inst, llvm::Instruction *PredInst,
                 const Context &PredCtx, const Context &CurrentCtx,
                 mono_container_t &IN, ResultTy *DF) {
    mono_container_t Incoming;

    const auto &PredOut = DF->OUT(PredInst, PredCtx);

    if (Problem.direction() ==
        ::dataflow::controlflow::FlowDirection::Backward) {
      if (llvm::isa<llvm::CallBase>(Inst) &&
          lotus::llvm_utils::isFunctionEntryInstruction(PredInst)) {
        const auto Callees = getCalleesForCall(Inst);
        bool Matches = false;
        for (auto *Callee : Callees) {
          if (Callee == PredInst->getFunction()) {
            Matches = true;
            break;
          }
        }
        Incoming =
            Matches ? Problem.callFlow(Inst, PredInst->getFunction(), PredOut)
                    : Problem.bottom();
      } else if (llvm::isa<llvm::CallBase>(Inst) &&
                 isContinuationOfCall(PredInst, Inst)) {
        const auto Callees = getCalleesForCall(Inst);
        Incoming = Problem.callToRetFlow(Inst, PredInst, Callees, PredOut);
      } else if (llvm::isa<llvm::ReturnInst>(Inst)) {
        if (!CurrentCtx.empty()) {
          auto ExitCtx = CurrentCtx;
          auto *CallSite = ExitCtx.pop_back();
          Incoming = Problem.returnFlow(CallSite, Inst->getFunction(), Inst,
                                        PredInst, PredOut);
        } else if (K == 0) {
          Incoming = Problem.bottom();
          if (ICF != nullptr) {
            bool FirstCaller = true;
            mono_container_t Merged;
            for (auto *Caller : ICF->getCallersOf(Inst->getFunction())) {
              auto RetFacts = Problem.returnFlow(Caller, Inst->getFunction(),
                                                 Inst, PredInst, PredOut);
              if (FirstCaller) {
                Merged = RetFacts;
                FirstCaller = false;
              } else {
                Merged = Problem.join(Merged, RetFacts);
              }
            }
            if (!FirstCaller) {
              Incoming = Merged;
            }
          }
        } else {
          // For K>0, [] is the ordinary root call string. Only K=0 collapses
          // caller histories into the empty context, so there is no return-flow
          // edge here unless the current context names the matching call site.
          Incoming = Problem.bottom();
        }
      } else {
        Incoming = PredOut;
      }
    } else if (lotus::llvm_utils::isFunctionEntryInstruction(Inst) &&
               llvm::isa<llvm::CallBase>(PredInst)) {
      // Call edge: PredInst is the call site, Inst is the callee entry.
      // Use callFlow to map caller facts to callee entry facts.
      const auto Callees = getCalleesForCall(PredInst);
      bool Matches = false;
      for (auto *Callee : Callees) {
        if (Callee == Inst->getFunction()) {
          Matches = true;
          break;
        }
      }
      if (Matches) {
        // Pass PredOut (= OUT[call site]) as the "In" to callFlow.
        Incoming = Problem.callFlow(PredInst, Inst->getFunction(), PredOut);
      } else {
        Incoming = Problem.bottom();
      }
    } else if (llvm::isa<llvm::ReturnInst>(PredInst)) {
      // Return edge: PredInst is a return instruction, Inst is the return site.
      if (!PredCtx.empty()) {
        Context CallerCtx = PredCtx;
        auto *CallSite = CallerCtx.pop_back();
        Incoming = Problem.returnFlow(CallSite, PredInst->getFunction(),
                                      PredInst, Inst, PredOut);
      } else if (K == 0) {
        // In K=0 mode the empty call string intentionally collapses all caller
        // contexts, so return-flow merges across every caller.
        Incoming = Problem.bottom();
        if (ICF != nullptr) {
          bool FirstCaller = true;
          mono_container_t Merged;
          for (auto *Caller : ICF->getCallersOf(PredInst->getFunction())) {
            auto RetFacts = Problem.returnFlow(Caller, PredInst->getFunction(),
                                               PredInst, Inst, PredOut);
            if (FirstCaller) {
              Merged = RetFacts;
              FirstCaller = false;
            } else {
              Merged = Problem.join(Merged, RetFacts);
            }
          }
          if (!FirstCaller) {
            Incoming = Merged;
          }
        }
      } else {
        // For K>0, [] is the ordinary root call string. Return-flow only
        // happens when the predecessor context still names the matching call
        // site exactly; only K=0 treats [] as a collapsed caller summary.
        Incoming = Problem.bottom();
      }
    } else if (llvm::isa<llvm::CallBase>(PredInst) &&
               isContinuationOfCall(Inst, PredInst)) {
      // Call-to-return edge: facts that bypass the callee.
      const auto Callees = getCalleesForCall(PredInst);
      Incoming = Problem.callToRetFlow(PredInst, Inst, Callees, PredOut);
    } else {
      Incoming = PredOut;
    }

    IN = Problem.join(IN, Incoming);
  }

  void computeOUT(llvm::Instruction *Inst, const Context &Ctx,
                  mono_container_t &OUT, ResultTy *DF) {
    // Apply the transfer function to produce OUT[Inst, Ctx] from IN[Inst, Ctx].
    // For call instructions the transfer function is still normalFlow (which
    // typically passes through facts unchanged); the call/return flow functions
    // are applied in computeIN at the callee entry / return site.
    OUT = Problem.normalFlow(Inst, DF->IN(Inst, Ctx));
  }

  std::vector<llvm::Function *>
  getCalleesForCall(llvm::Instruction *Inst) const {
    if (ICF == nullptr) {
      return Problem.getCalleesOfCallAt(Inst);
    }
    std::vector<llvm::Function *> Callees;
    for (auto *Callee : ICF->getCalleesOfCallAt(Inst)) {
      if (Callee != nullptr) {
        Callees.push_back(Callee);
      }
    }
    return Callees;
  }

  ProblemTy &Problem;
  std::unique_ptr<ResultTy> Result;
  std::unique_ptr<::dataflow::controlflow::LLVMInterCFG> OwnedICF;
  const ICFG *ICF = nullptr;
};

template <typename Problem, unsigned K>
using InterMonoSolver_P =
    InterMonoSolver<typename Problem::ProblemAnalysisTypes, K>;

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_SOLVER_INTERSOLVER_H_
