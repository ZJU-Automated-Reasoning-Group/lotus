/*
 * lotus-dfa-npa
 *
 * Dataflow testing tool: NPA engine.
 */

#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instruction.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/NPA/LLVM/BitVectorSolver.h"
#include "Dataflow/NPA/LLVM/AnalysisSupport.h"
#include "Dataflow/NPA/Analyses/Inter/ConstantPropagation.h"
#include "Dataflow/NPA/Analyses/Inter/Interval.h"
#include "Dataflow/NPA/Analyses/Inter/LiveVariables.h"
#include "Dataflow/NPA/Analyses/Inter/MaybeUninitialized.h"
#include "Dataflow/NPA/Analyses/Inter/Nullability.h"
#include "Dataflow/NPA/Analyses/Inter/ReachingDefinitions.h"
#include "Dataflow/NPA/Analyses/Intra/LiveVariables.h"
#include "Dataflow/NPA/Analyses/Intra/ReachableBlocks.h"
#include "Dataflow/NPA/Analyses/Intra/ReachingDefinitions.h"
#include "ToolSupport.h"
#include "Utils/Parallel/ThreadPool.h"

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

using namespace llvm;

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<bitcode>"),
                                          cl::Required);
static cl::opt<std::string> OutDir("out-dir", cl::desc("Output directory"),
                                   cl::value_desc("dir"), cl::init(""));
static cl::opt<bool> StdoutOpt(
    "stdout",
    cl::desc("Write analysis results to stdout when --out-dir is not set"),
    cl::init(false));
static cl::opt<std::string>
    AnalysisOpt("analysis",
                cl::desc("Analysis: liveness (default), reaching_defs, "
                         "reachable, inter_liveness, inter_reaching_defs, "
                         "inter_uninitialized, constant_prop, interval, "
                         "nullability"),
                cl::init("liveness"));
static cl::opt<std::string>
    SolverOpt("solver", cl::desc("Solver: newton (default), kleene"),
              cl::init("newton"));
static cl::opt<std::string> LinearSolverOpt(
    "linear-solver",
    cl::desc("Newton linear solver: scc (default), adaptive_scc, tensor"),
    cl::init("scc"));

namespace {

using lotus::dataflow_tool::FunctionView;
using ModuleValueIdMap = std::unordered_map<const Value *, std::string>;

struct BlockView final {
  Function &Func;
  FunctionView InstView;
  std::vector<const BasicBlock *> OrderedBlocks;
  std::unordered_map<const BasicBlock *, std::string> BlockToId;
  std::vector<std::string> BitLabels;
};

struct ModuleView final {
  ModuleValueIdMap ValueToId;
  std::vector<std::string> BitLabels;
  std::vector<Function *> OrderedFunctions;
};

BlockView buildBlockView(Function &F) {
  BlockView View{F, lotus::dataflow_tool::buildFunctionView(F), {}, {}, {}};

  unsigned BlockIdx = 0;
  for (auto &BB : F) {
    View.OrderedBlocks.push_back(&BB);
    View.BlockToId[&BB] = "bb" + std::to_string(BlockIdx++);
  }

  for (auto &Arg : F.args())
    View.BitLabels.push_back(View.InstView.ValueToId.at(&Arg));

  for (auto *I : View.InstView.OrderedInsts) {
    if (!I->getType()->isVoidTy())
      View.BitLabels.push_back(View.InstView.ValueToId.at(I));
  }

  if (View.BitLabels.empty())
    View.BitLabels.push_back("bit0");

  return View;
}

ModuleView buildModuleView(Module &M) {
  ModuleView View;

  unsigned ArgIdx = 0;
  unsigned InstIdx = 0;
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    View.OrderedFunctions.push_back(&F);
    for (auto &Arg : F.args()) {
      std::string Id = "arg" + std::to_string(ArgIdx++);
      View.ValueToId.emplace(&Arg, Id);
      View.BitLabels.push_back(Id);
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (I.getType()->isVoidTy())
          continue;
        std::string Id = "i" + std::to_string(InstIdx++);
        View.ValueToId.emplace(&I, Id);
        View.BitLabels.push_back(Id);
      }
    }
  }

  if (View.BitLabels.empty())
    View.BitLabels.push_back("bit0");

  return View;
}

std::string lookupValueId(const Value *V, const ModuleValueIdMap &ValueToId,
                          StringRef Fallback = "v") {
  auto It = ValueToId.find(V);
  if (It != ValueToId.end())
    return It->second;
  return std::string(Fallback);
}

std::string formatAPInt(const APInt &Value, bool Signed = true) {
  std::string Buffer;
  raw_string_ostream OS(Buffer);
  Value.print(OS, Signed);
  return OS.str();
}

std::string
formatConstantPropagationValue(const npa::ConstantPropagationValue &Value) {
  if (!Value.isConstant())
    return "top";
  return "const(" + formatAPInt(Value.constant) + ")";
}

std::string formatInterval(const npa::Interval &Interval) {
  if (Interval.bottom)
    return "bottom";
  std::string Lower = Interval.hasLower ? formatAPInt(Interval.lower) : "-inf";
  std::string Upper = Interval.hasUpper ? formatAPInt(Interval.upper) : "+inf";
  return (Twine(Interval.ordering == npa::IntervalOrdering::Unsigned ? "u"
                                                                     : "s") +
          "[" + Lower + "," + Upper + "]")
      .str();
}

std::vector<std::string>
buildMaybeUninitializedLabels(Module &M, const ModuleValueIdMap &ValueToId) {
  std::vector<std::string> Labels;
  std::unordered_map<const Value *, unsigned> ValueBits;
  std::unordered_map<const Value *, unsigned> MemoryBits;
  auto addValueBit = [&](const Value *V) {
    if (!V || ValueBits.count(V))
      return;
    ValueBits.emplace(V, static_cast<unsigned>(Labels.size()));
    Labels.push_back(lookupValueId(V, ValueToId));
  };
  auto addMemoryBit = [&](const Value *V) {
    if (!V || !V->getType()->isPointerTy())
      return;
    const Value *Memory = getUnderlyingObject(V->stripPointerCasts());
    if (!Memory || MemoryBits.count(Memory))
      return;
    MemoryBits.emplace(Memory, static_cast<unsigned>(Labels.size()));
    Labels.push_back("mem(" + lookupValueId(Memory, ValueToId) + ")");
  };

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    for (auto &Arg : F.args()) {
      addValueBit(&Arg);
      addMemoryBit(&Arg);
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (!I.getType()->isVoidTy())
          addValueBit(&I);
        if (isa<AllocaInst>(&I))
          addMemoryBit(&I);
        if (auto *Load = dyn_cast<LoadInst>(&I))
          addMemoryBit(Load->getPointerOperand());
        if (auto *Store = dyn_cast<StoreInst>(&I))
          addMemoryBit(Store->getPointerOperand());
        if (auto *Call = dyn_cast<CallBase>(&I)) {
          for (Use &Arg : Call->args())
            addMemoryBit(Arg.get());
        }
      }
    }
  }

  if (Labels.empty())
    Labels.push_back("bit0");
  return Labels;
}

template <typename BitMapT>
void assignBitLabels(std::vector<std::string> &Labels, const BitMapT &Bits,
                     const ModuleValueIdMap &ValueToId, StringRef Prefix) {
  for (const auto &Entry : Bits) {
    if (Entry.second >= Labels.size())
      Labels.resize(Entry.second + 1);
    std::string Label =
        Prefix.empty()
            ? lookupValueId(Entry.first, ValueToId)
            : (Prefix + "(" + lookupValueId(Entry.first, ValueToId) + ")")
                  .str();
    if (Labels[Entry.second].empty() || Label < Labels[Entry.second])
      Labels[Entry.second] = std::move(Label);
  }
}

template <typename BitMapT>
void assignBitVectorLabels(std::vector<std::string> &Labels,
                           const BitMapT &Bits,
                           const ModuleValueIdMap &ValueToId,
                           StringRef Prefix) {
  for (const auto &Entry : Bits) {
    for (unsigned Bit : Entry.second) {
      if (Bit >= Labels.size())
        Labels.resize(Bit + 1);
      std::string Label =
          Prefix.empty()
              ? lookupValueId(Entry.first, ValueToId)
              : (Prefix + "(" + lookupValueId(Entry.first, ValueToId) + ")")
                    .str();
      if (Labels[Bit].empty() || Label < Labels[Bit])
        Labels[Bit] = std::move(Label);
    }
  }
}

void finalizeBitLabels(std::vector<std::string> &Labels) {
  if (Labels.empty()) {
    Labels.push_back("bit0");
    return;
  }
  for (size_t I = 0; I < Labels.size(); ++I) {
    if (Labels[I].empty())
      Labels[I] = "bit" + std::to_string(I);
  }
}

npa::SolverStrategy parseSolverStrategy(StringRef Name) {
  if (Name == "kleene")
    return npa::SolverStrategy::Kleene;
  return npa::SolverStrategy::Newton;
}

npa::LinearStrategy parseLinearStrategy(StringRef Name) {
  if (Name == "adaptive_scc")
    return npa::LinearStrategy::AdaptiveScc;
  if (Name == "tensor")
    return npa::LinearStrategy::TensorProduct;
  return npa::LinearStrategy::SCC;
}

void formatBitSet(raw_ostream &OS, const APInt &Bits,
                  const std::vector<std::string> &BitLabels) {
  bool First = true;
  const unsigned Width =
      std::min<unsigned>(Bits.getBitWidth(), BitLabels.size());
  for (unsigned Bit = 0; Bit < Width; ++Bit) {
    if (!Bits[Bit])
      continue;
    if (!First)
      OS << ",";
    OS << BitLabels[Bit];
    First = false;
  }
}

template <typename Printer>
void printBlockStates(raw_ostream &OS, const BlockView &View,
                      Printer &&PrintState) {
  for (const BasicBlock *BB : View.OrderedBlocks) {
    OS << "  " << View.BlockToId.at(BB) << " IN: ";
    PrintState(BB);
    OS << "\n";
  }
}

void runLiveness(raw_ostream &OS, Function &F, npa::SolverStrategy Strategy,
                 npa::LinearStrategy LinearStrategy) {
  BlockView View = buildBlockView(F);
  auto Result = npa::LiveVariables::run(F, Strategy, LinearStrategy);
  lotus::dataflow_tool::emitFunctionHeader(OS, F);
  printBlockStates(OS, View, [&](const BasicBlock *BB) {
    auto It = Result.IN.find(BB);
    if (It != Result.IN.end())
      formatBitSet(OS, It->second, View.BitLabels);
  });
}

void runReachingDefinitions(raw_ostream &OS, Function &F,
                            npa::SolverStrategy Strategy,
                            npa::LinearStrategy LinearStrategy) {
  BlockView View = buildBlockView(F);
  auto Result = npa::ReachingDefinitions::run(F, Strategy, LinearStrategy);
  lotus::dataflow_tool::emitFunctionHeader(OS, F);
  printBlockStates(OS, View, [&](const BasicBlock *BB) {
    auto It = Result.IN.find(BB);
    if (It != Result.IN.end())
      formatBitSet(OS, It->second, View.BitLabels);
  });
}

void runReachable(raw_ostream &OS, Function &F, npa::SolverStrategy Strategy,
                  npa::LinearStrategy LinearStrategy) {
  BlockView View = buildBlockView(F);
  auto Reachable = npa::ReachableBlocks::run(F, Strategy, LinearStrategy);
  lotus::dataflow_tool::emitFunctionHeader(OS, F);
  printBlockStates(OS, View, [&](const BasicBlock *BB) {
    OS << (Reachable.count(BB) ? "reachable" : "unreachable");
  });
}

template <typename Printer>
void printModuleBlockStates(raw_ostream &OS, Module &M, Printer &&PrintState) {
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    BlockView View = buildBlockView(F);
    lotus::dataflow_tool::emitFunctionHeader(OS, F);
    for (const BasicBlock *BB : View.OrderedBlocks) {
      OS << "  " << View.BlockToId.at(BB) << " IN: ";
      PrintState(BB);
      OS << "\n";
    }
  }
}

void runInterproceduralLiveness(raw_ostream &OS, Module &M,
                                npa::LinearStrategy LinearStrategy) {
  const ModuleView View = buildModuleView(M);
  auto Result = npa::InterLiveVariables::run(M, false, LinearStrategy);
  OS << "  [profile] phase=artifact_construction seconds="
     << Result.status.phase_artifact_construction_time << "\n";
  OS << "  [profile] phase=summary_solve seconds="
     << Result.status.summary_solve.time << "\n";
  OS << "  [profile] phase=summary_materialization seconds="
     << Result.status.phase_summary_materialization_time << "\n";
  OS << "  [profile] phase=propagation seconds="
     << Result.status.phase_propagation_time << "\n";
  printModuleBlockStates(OS, M, [&](const BasicBlock *BB) {
    auto It = Result.blockFacts.find(npa::BlockKey{BB});
    if (It != Result.blockFacts.end())
      formatBitSet(OS, It->second, View.BitLabels);
  });
}

void runInterproceduralReachingDefinitions(raw_ostream &OS, Module &M,
                                           npa::LinearStrategy LinearStrategy) {
  const ModuleView View = buildModuleView(M);
  auto Result = npa::InterReachingDefinitions::run(M, false, LinearStrategy);
  OS << "  [profile] phase=artifact_construction seconds="
     << Result.status.phase_artifact_construction_time << "\n";
  OS << "  [profile] phase=summary_solve seconds="
     << Result.status.summary_solve.time << "\n";
  OS << "  [profile] phase=summary_materialization seconds="
     << Result.status.phase_summary_materialization_time << "\n";
  OS << "  [profile] phase=propagation seconds="
     << Result.status.phase_propagation_time << "\n";
  printModuleBlockStates(OS, M, [&](const BasicBlock *BB) {
    auto It = Result.blockFacts.find(npa::BlockKey{BB});
    if (It != Result.blockFacts.end())
      formatBitSet(OS, It->second, View.BitLabels);
  });
}

void runInterproceduralMaybeUninitialized(raw_ostream &OS, Module &M,
                                          npa::LinearStrategy LinearStrategy) {
  const ModuleView View = buildModuleView(M);
  const auto Labels = buildMaybeUninitializedLabels(M, View.ValueToId);
  auto Result = npa::InterMaybeUninitialized::run(M, false, LinearStrategy);
  OS << "  [profile] phase=artifact_construction seconds="
     << Result.status.phase_artifact_construction_time << "\n";
  OS << "  [profile] phase=summary_solve seconds="
     << Result.status.summary_solve.time << "\n";
  OS << "  [profile] phase=summary_materialization seconds="
     << Result.status.phase_summary_materialization_time << "\n";
  OS << "  [profile] phase=propagation seconds="
     << Result.status.phase_propagation_time << "\n";
  printModuleBlockStates(OS, M, [&](const BasicBlock *BB) {
    auto It = Result.blockFacts.find(npa::BlockKey{BB});
    if (It != Result.blockFacts.end())
      formatBitSet(OS, It->second, Labels);
  });
}

void runInterproceduralConstantPropagation(raw_ostream &OS, Module &M,
                                           npa::LinearStrategy LinearStrategy) {
  const ModuleView View = buildModuleView(M);
  auto Result = npa::InterConstantPropagation::run(M, false, LinearStrategy);
  OS << "  [profile] phase=artifact_construction seconds="
     << Result.status.phase_artifact_construction_time << "\n";
  OS << "  [profile] phase=summary_solve seconds="
     << Result.status.summary_solve.time << "\n";
  OS << "  [profile] phase=summary_materialization seconds="
     << Result.status.phase_summary_materialization_time << "\n";
  OS << "  [profile] phase=propagation seconds="
     << Result.status.phase_propagation_time << "\n";
  printModuleBlockStates(OS, M, [&](const BasicBlock *BB) {
    auto It = Result.blockFacts.find(npa::BlockKey{BB});
    if (It == Result.blockFacts.end())
      return;
    OS << (It->second.reachable ? "reachable:" : "unreachable:");
    lotus::dataflow_tool::formatValueMap(
        OS, It->second.values, View.ValueToId,
        [](const npa::ConstantPropagationValue &Value) {
          return formatConstantPropagationValue(Value);
        });
  });
}

void runInterproceduralInterval(raw_ostream &OS, Module &M,
                                npa::LinearStrategy LinearStrategy) {
  const ModuleView View = buildModuleView(M);
  auto Result = npa::InterIntervalAnalysis::run(M, false, LinearStrategy);
  OS << "  [profile] phase=artifact_construction seconds="
     << Result.status.phase_artifact_construction_time << "\n";
  OS << "  [profile] phase=summary_solve seconds="
     << Result.status.summary_solve.time << "\n";
  OS << "  [profile] phase=summary_materialization seconds="
     << Result.status.phase_summary_materialization_time << "\n";
  OS << "  [profile] phase=propagation seconds="
     << Result.status.phase_propagation_time << "\n";
  printModuleBlockStates(OS, M, [&](const BasicBlock *BB) {
    auto It = Result.blockFacts.find(npa::BlockKey{BB});
    if (It == Result.blockFacts.end())
      return;
    OS << (It->second.reachable ? "reachable:" : "unreachable:");
    lotus::dataflow_tool::formatValueMap(
        OS, It->second.values, View.ValueToId,
        [](const npa::Interval &Interval) { return formatInterval(Interval); });
  });
}

void runInterproceduralNullability(raw_ostream &OS, Module &M,
                                   npa::LinearStrategy LinearStrategy) {
  const ModuleView View = buildModuleView(M);
  auto Result = npa::InterNullability::run(M, false, LinearStrategy);
  OS << "  [profile] phase=artifact_construction seconds="
     << Result.status.phase_artifact_construction_time << "\n";
  OS << "  [profile] phase=summary_solve seconds="
     << Result.status.summary_solve.time << "\n";
  OS << "  [profile] phase=summary_materialization seconds="
     << Result.status.phase_summary_materialization_time << "\n";
  OS << "  [profile] phase=propagation seconds="
     << Result.status.phase_propagation_time << "\n";
  std::vector<std::string> Labels;
  assignBitLabels(Labels, Result.valueBits, View.ValueToId, "");
  assignBitLabels(Labels, Result.memoryBits, View.ValueToId, "mem");
  assignBitVectorLabels(Labels, Result.pointerMemoryBits, View.ValueToId,
                        "mem");
  finalizeBitLabels(Labels);
  printModuleBlockStates(OS, M, [&](const BasicBlock *BB) {
    auto It = Result.blockFacts.find(npa::BlockKey{BB});
    if (It != Result.blockFacts.end())
      formatBitSet(OS, It->second, Labels);
  });
}

struct AnalysisHandler final {
  StringRef Name;
  bool ModuleScoped = false;
  void (*RunFunction)(raw_ostream &, Function &, npa::SolverStrategy,
                      npa::LinearStrategy) = nullptr;
  void (*RunModule)(raw_ostream &, Module &, npa::LinearStrategy) = nullptr;
};

const AnalysisHandler Handlers[] = {
    {"liveness", false, &runLiveness, nullptr},
    {"reaching_defs", false, &runReachingDefinitions, nullptr},
    {"reachable", false, &runReachable, nullptr},
    {"inter_liveness", true, nullptr, &runInterproceduralLiveness},
    {"inter_reaching_defs", true, nullptr,
     &runInterproceduralReachingDefinitions},
    {"inter_uninitialized", true, nullptr,
     &runInterproceduralMaybeUninitialized},
    {"inter_constant_prop", true, nullptr,
     &runInterproceduralConstantPropagation},
    {"inter_interval", true, nullptr, &runInterproceduralInterval},
    {"inter_nullability", true, nullptr, &runInterproceduralNullability},
};

std::string runAnalysisToString(const AnalysisHandler &Handler, Function &F,
                                npa::SolverStrategy Strategy,
                                npa::LinearStrategy LinearStrategy) {
  std::string Buffer;
  raw_string_ostream FunctionOS(Buffer);
  Handler.RunFunction(FunctionOS, F, Strategy, LinearStrategy);
  return FunctionOS.str();
}

void runIntraproceduralAnalysesOnModule(raw_ostream &OS, Module &M,
                                        const AnalysisHandler &Handler,
                                        npa::SolverStrategy Strategy,
                                        npa::LinearStrategy LinearStrategy) {
  assert(!Handler.ModuleScoped &&
         "module-scoped interprocedural analyses schedule inside the engine");
  std::vector<Function *> Functions;
  for (auto &F : M) {
    if (!F.isDeclaration())
      Functions.push_back(&F);
  }

  std::vector<std::string> Outputs(Functions.size());
  ThreadPool *Pool = ThreadPool::get();
  const bool ParallelFunctions =
      Pool->workerCount() > 1 && Functions.size() > 1;

  if (ParallelFunctions) {
    const std::size_t GrainSize = npa::detail::parallel_task_grain_size(
        Functions.size(), Pool->workerCount(), 2);
    Pool->parallelFor<std::size_t>(
        0, Functions.size(), GrainSize, [&](std::size_t Index) {
          Outputs[Index] = runAnalysisToString(Handler, *Functions[Index],
                                               Strategy, LinearStrategy);
        });
  } else {
    for (std::size_t Index = 0; Index < Functions.size(); ++Index)
      Outputs[Index] = runAnalysisToString(Handler, *Functions[Index], Strategy,
                                           LinearStrategy);
  }

  for (const auto &Output : Outputs)
    OS << Output;
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(
      argc, argv,
      "NPA engine testing\n"
      "Use -nworkers=<N> to enable intraprocedural module scheduling and "
      "eligible NPA/internal interprocedural parallel execution.\n");

  if (SolverOpt != "newton" && SolverOpt != "kleene") {
    errs() << "error: unknown NPA solver '" << SolverOpt << "'\n";
    return 1;
  }
  if (LinearSolverOpt != "scc" && LinearSolverOpt != "adaptive_scc" &&
      LinearSolverOpt != "tensor") {
    errs() << "error: unknown NPA linear solver '" << LinearSolverOpt << "'\n";
    return 1;
  }

  LLVMContext Context;
  SMDiagnostic Err;
  auto M = lotus::dataflow_tool::loadModuleOrReport(InputFilename, Context, Err,
                                                    argv[0]);
  if (!M)
    return 1;

  lotus::dataflow_tool::prepareModule(*M);

  raw_null_ostream NullOS;
  std::unique_ptr<raw_fd_ostream> FileOS;
  std::error_code EC;
  raw_ostream &OS = lotus::dataflow_tool::selectOutputStream(
      StdoutOpt, OutDir, "npa.txt", FileOS, NullOS, EC);
  if (EC) {
    errs() << "error: cannot create " << OutDir << "/npa.txt: " << EC.message()
           << "\n";
    return 1;
  }

  const auto *Handler =
      lotus::dataflow_tool::findHandler(AnalysisOpt, Handlers);
  if (!Handler) {
    errs() << "error: unknown NPA analysis '" << AnalysisOpt << "'\n";
    return 1;
  }

  if (Handler->ModuleScoped && SolverOpt != "newton") {
    errs() << "error: NPA analysis '" << AnalysisOpt
           << "' uses the module-level interprocedural engine and does not "
              "support --solver="
           << SolverOpt << "\n";
    return 1;
  }

  const npa::SolverStrategy Strategy = parseSolverStrategy(SolverOpt);
  const npa::LinearStrategy LinearStrategy = parseLinearStrategy(LinearSolverOpt);
  const unsigned WorkerCount = ThreadPool::get()->workerCount();
  const bool ParallelEnabled = WorkerCount > 1;
  OS << "[npa:" << AnalysisOpt;
  if (Handler->ModuleScoped)
    OS << ":module";
  else
    OS << ":" << SolverOpt;
  OS << ":linear=" << LinearSolverOpt << ":workers=" << WorkerCount
     << ":parallel=" << (ParallelEnabled ? "on" : "off") << "]\n";
  if (Handler->ModuleScoped)
    Handler->RunModule(OS, *M, LinearStrategy);
  else
    runIntraproceduralAnalysesOnModule(OS, *M, *Handler, Strategy,
                                       LinearStrategy);

  return 0;
}
