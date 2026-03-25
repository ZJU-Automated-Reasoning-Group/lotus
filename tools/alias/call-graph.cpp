/******************************************************************************

 *****************************************************************************/

#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include "Alias/AliasAnalysisWrapper/CLIUtils.h"
#include "Alias/AserPTA/PointerAnalysis/Context/KCallSite.h"
#include "Alias/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
#include "Alias/AserPTA/PointerAnalysis/PointerAnalysisPass.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/WavePropagation.h"
#include "Alias/AserPTA/PreProcessing/Passes/CanonicalizeGEPPass.h"
#include "Alias/AserPTA/PreProcessing/Passes/LoweringMemCpyPass.h"
#include "Alias/AserPTA/PreProcessing/Passes/RemoveASMInstPass.h"
#include "Alias/AserPTA/PreProcessing/Passes/RemoveExceptionHandlerPass.h"
#include "Alias/AserPTA/PreProcessing/Passes/StandardHeapAPIRewritePass.h"
#include "Alias/AserPTA/Util/Log.h"
#include "Alias/DFPA/DFPAPass.h"
#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckCallGraph.h"
#include "Alias/DyckAA/DyckCallGraphNode.h"
#include "Alias/FPA/CallGraphPass.h"
#include "Alias/FPA/Common.h"
#include "Alias/FPA/Config.h"
#include "Alias/FPA/FLTAPass.h"
#include "Alias/FPA/KELPPass.h"
#include "Alias/FPA/MLTADFPass.h"
#include "Alias/FPA/MLTAPass.h"
#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/LotusAA/Support/FunctionPointerResults.h"

#include <chrono>
#include <map>
#include <memory>
#include <system_error>
#include <vector>

namespace cl = llvm::cl;

static cl::OptionCategory CGCat("CallGraph");

enum class CGType {
  DyckAA,
  LotusAA,
  DFPA,
  FPA_FLTA,
  FPA_MLTA,
  FPA_MLTADF,
  FPA_KELP,
  AserPTA_CI,
  AserPTA_1CFA,
  AserPTA_2CFA
};

static cl::opt<CGType> AnalysisType(
    "cg-type", cl::desc("Call-graph analysis type"),
    cl::values(
        clEnumValN(CGType::DyckAA, "dyck", "DyckAA"),
        clEnumValN(CGType::LotusAA, "lotus", "LotusAA"),
        clEnumValN(CGType::DFPA, "dfpa",
                   "Demand-refined function pointer analysis"),
        clEnumValN(CGType::FPA_FLTA, "fpa-flta", "FPA FLTA"),
        clEnumValN(CGType::FPA_MLTA, "fpa-mlta", "FPA MLTA"),
        clEnumValN(CGType::FPA_MLTADF, "fpa-mltadf", "FPA MLTA+DF"),
        clEnumValN(CGType::FPA_KELP, "fpa-kelp", "FPA KELP"),
        clEnumValN(CGType::AserPTA_CI, "aserpta-ci",
                   "AserPTA (Context-Insensitive)"),
        clEnumValN(CGType::AserPTA_1CFA, "aserpta-1cfa", "AserPTA (1-CFA)"),
        clEnumValN(CGType::AserPTA_2CFA, "aserpta-2cfa", "AserPTA (2-CFA)")),
    cl::init(CGType::DyckAA), cl::cat(CGCat));

static cl::opt<bool>
    EmitCGAsDot("emit-cg-as-dot",
                cl::desc("Output call-graph as DOT (default: true)"),
                cl::init(true), cl::cat(CGCat));
static cl::opt<bool> EmitCGAsJson("emit-cg-as-json",
                                  cl::desc("Output call-graph as JSON"),
                                  cl::cat(CGCat));
static cl::opt<std::string>
    OutputFile("o", cl::desc("Output file (default: stdout)"), cl::init("-"),
               cl::cat(CGCat));
static cl::opt<bool> EmitStats("S", cl::desc("Compute statistics"),
                               cl::cat(CGCat));
static cl::opt<std::string> IRFile(cl::Positional, cl::Required,
                                   cl::desc("<LLVM IR file>"), cl::cat(CGCat));
static cl::opt<int> FPAMaxTypeLayer("fpa-max-type-layer",
                                    cl::desc("Max type layer for FPA"),
                                    cl::init(10), cl::cat(CGCat));
static cl::opt<unsigned long long>
    DFPAMaxDemandStates("dfpa-max-demand-states",
                        cl::desc("Demand-state budget for DFPA"),
                        cl::init(50000), cl::cat(CGCat));
static cl::opt<unsigned>
    DFPAIndirectCtxK("dfpa-indirect-ctx-k",
                     cl::desc("Selective indirect-edge context depth for DFPA"),
                     cl::init(1), cl::cat(CGCat));

struct DiagTimer {
  std::chrono::steady_clock::time_point Start;
  llvm::StringRef Message;
  DiagTimer(llvm::StringRef Msg) : Message(Msg) {
    Start = std::chrono::steady_clock::now();
  }
  ~DiagTimer() {
    auto Elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - Start)
                       .count() /
                   1000.0;
    llvm::errs() << Message << " (" << Elapsed << "s)\n";
  }
  double elapsed() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - Start)
               .count() /
           1000.0;
  }
};

// Forward declarations
static void computeCGStats(llvm::CallGraph &CG, llvm::raw_ostream &OS);
static void printCGAsDot(llvm::CallGraph &CG, llvm::raw_ostream &OS);
static void printCGAsJson(llvm::CallGraph &CG, llvm::raw_ostream &OS);

// Helper: Add call edge if valid
static void addCallEdge(llvm::CallGraph &CG, llvm::Function *Caller,
                        llvm::CallBase *CS, llvm::Function *Callee) {
  if (!Caller || !Callee || Callee->isDeclaration())
    return;
  if (auto *Node = CG[Caller]) {
    for (auto &Entry : *Node) {
      if (Entry.first.hasValue() && Entry.first.getValue() == CS &&
          Entry.second == CG[Callee])
        return;
    }
    Node->addCalledFunction(CS, CG[Callee]);
  }
}

// Helper: Process all calls in a function
static void processDirectCalls(llvm::Module &M, llvm::CallGraph &CG) {
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto *CB = dyn_cast<llvm::CallBase>(&I)) {
          if (auto *Callee = CB->getCalledFunction()) {
            addCallEdge(CG, &F, CB, Callee);
          }
        }
      }
    }
  }
}

// Build call graph using DyckAA
static void buildCGWithDyckAA(llvm::Module &M, llvm::CallGraph &CG) {
  llvm::legacy::PassManager PM;
  auto *DyckAA = new DyckAliasAnalysis();
  PM.add(DyckAA);
  PM.run(M);

  if (auto *DyckCG = DyckAA->getDyckCallGraph()) {
    for (auto NodeIt = DyckCG->nodes_begin(); NodeIt != DyckCG->nodes_end();
         ++NodeIt) {
      auto *Node = *NodeIt;
      auto *Caller = Node->getLLVMFunction();
      if (!Caller || Caller->isDeclaration())
        continue;

      auto *CGNode = CG[Caller];
      if (!CGNode)
        continue;

      for (auto CCIt = Node->common_call_begin();
           CCIt != Node->common_call_end(); ++CCIt) {
        if (auto *CB = dyn_cast<llvm::CallBase>((*CCIt)->getInstruction())) {
          if (auto *Callee = (*CCIt)->getCalledFunction()) {
            CGNode->addCalledFunction(CB, CG[Callee]);
          }
        }
      }

      for (auto PCIt = Node->pointer_call_begin();
           PCIt != Node->pointer_call_end(); ++PCIt) {
        if (auto *CB = dyn_cast<llvm::CallBase>((*PCIt)->getInstruction())) {
          for (auto *Callee : **PCIt) {
            addCallEdge(CG, Caller, CB, Callee);
          }
        }
      }
    }
  }
}

// Build call graph using LotusAA
static void buildCGWithLotusAA(llvm::Module &M, llvm::CallGraph &CG) {
  // Initialize passes
  llvm::PassRegistry &Registry = *llvm::PassRegistry::getPassRegistry();
  llvm::initializeCore(Registry);
  llvm::initializeAnalysis(Registry);

  llvm::legacy::PassManager PM;
  auto *LotusAAPass = new LotusAA();
  PM.add(LotusAAPass);
  PM.run(M);

  processDirectCalls(M, CG);

  const auto &Results =
      LotusAAPass->getFunctionPointerResults().getResultsMap();
  for (const auto &CallerResults : Results) {
    llvm::Function *Caller = CallerResults.first;
    if (!Caller || Caller->isDeclaration())
      continue;
    auto *CGNode = CG[Caller];
    if (!CGNode)
      continue;

    for (const auto &CallSiteResults : CallerResults.second) {
      llvm::Value *CallSite = CallSiteResults.first;
      const auto &Targets = CallSiteResults.second;

      llvm::CallBase *CB = dyn_cast<llvm::CallBase>(CallSite);
      if (!CB) {
        // Find matching CallBase in function
        for (auto &BB : *Caller) {
          for (auto &I : BB) {
            if ((CB = dyn_cast<llvm::CallBase>(&I)) &&
                (CB == CallSite || CB->getCalledOperand() == CallSite)) {
              break;
            }
          }
          if (CB)
            break;
        }
      }
      if (CB) {
        for (const auto &TargetItem : Targets) {
          addCallEdge(CG, Caller, CB, TargetItem.first);
        }
      }
    }
  }
}

// Build call graph using AserPTA (template for different context sensitivities)
template <typename Context>
static void buildCGWithAserPTAImpl(llvm::Module &M, llvm::CallGraph &CG) {
  using namespace aser;
  using Solver =
      WavePropagation<DefaultLangModel<Context, FSMemModel<Context>>>;

  llvm::legacy::PassManager PM;
  PM.add(new CanonicalizeGEPPass());
  PM.add(new LoweringMemCpyPass());
  PM.add(new RemoveExceptionHandlerPass());
  PM.add(new RemoveASMInstPass());
  PM.add(new StandardHeapAPIRewritePass());

  auto *PTAPass = new PointerAnalysisPass<Solver>();
  PM.add(PTAPass);
  PM.run(M);

  processDirectCalls(M, CG);

  if (auto *Solver = PTAPass->getPTA()) {
    if (auto *AserCG = Solver->getCallGraph()) {
      for (auto NodeIt = AserCG->begin(); NodeIt != AserCG->end(); ++NodeIt) {
        auto *CGNode = *NodeIt;
        if (CGNode && CGNode->isIndirectCall()) {
          if (auto *IndCall = CGNode->getTargetFunPtr()) {
            if (auto *CallInst =
                    dyn_cast<llvm::CallBase>(IndCall->getCallSite())) {
              auto *Caller =
                  const_cast<llvm::Function *>(CallInst->getFunction());
              if (Caller && !Caller->isDeclaration()) {
                auto *NonConstCallInst = const_cast<llvm::CallBase *>(CallInst);
                for (auto *Resolved : IndCall->getResolvedNode()) {
                  if (Resolved && !Resolved->isIndirectCall()) {
                    if (auto *TargetFun = Resolved->getTargetFun()) {
                      addCallEdge(CG, Caller, NonConstCallInst,
                                  const_cast<llvm::Function *>(
                                      TargetFun->getFunction()));
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  PTAPass->release();
}

// Build call graph using AserPTA (wrapper to select context sensitivity)
static void buildCGWithAserPTA(llvm::Module &M, llvm::CallGraph &CG,
                               CGType Type) {
  using namespace aser;
  switch (Type) {
  case CGType::AserPTA_CI:
    buildCGWithAserPTAImpl<NoCtx>(M, CG);
    break;
  case CGType::AserPTA_1CFA:
    buildCGWithAserPTAImpl<KCallSite<1>>(M, CG);
    break;
  case CGType::AserPTA_2CFA:
    buildCGWithAserPTAImpl<KCallSite<2>>(M, CG);
    break;
  default:
    llvm::WithColor::error() << "Invalid AserPTA variant\n";
    break;
  }
}

// Build call graph using FPA
static void buildCGWithFPA(llvm::Module &M, llvm::CallGraph &CG, CGType Type) {
  GlobalContext GlobalCtx;
  ModuleList Modules = {{&M, M.getName()}};
  GlobalCtx.Modules = std::move(Modules);
  GlobalCtx.ModuleMaps.insert({&M, M.getName()});

  debug_mode = false;
  max_type_layer = FPAMaxTypeLayer;

  CallGraphPass *Pass = nullptr;
  switch (Type) {
  case CGType::FPA_FLTA:
    Pass = new FLTAPass(&GlobalCtx);
    break;
  case CGType::FPA_MLTA:
    Pass = new MLTAPass(&GlobalCtx);
    break;
  case CGType::FPA_MLTADF:
    Pass = new MLTADFPass(&GlobalCtx);
    break;
  case CGType::FPA_KELP:
    Pass = new KELPPass(&GlobalCtx);
    break;
  default:
    llvm::report_fatal_error("Invalid FPA type");
  }

  Pass->run(Modules);

  for (const auto &CallSiteResults : GlobalCtx.Callees) {
    llvm::CallInst *CI = CallSiteResults.first;
    if (CI && CI->getFunction() && !CI->getFunction()->isDeclaration()) {
      const FuncSet &Targets = CallSiteResults.second;
      for (auto *Callee : Targets) {
        addCallEdge(CG, CI->getFunction(), CI, Callee);
      }
    }
  }
}

static void buildCGWithDFPA(llvm::Module &M, llvm::CallGraph &CG) {
  llvm::legacy::PassManager PM;
  PM.add(new aser::CanonicalizeGEPPass());
  PM.add(new aser::LoweringMemCpyPass());
  PM.add(new aser::RemoveExceptionHandlerPass());
  PM.add(new aser::RemoveASMInstPass());
  PM.add(new StandardHeapAPIRewritePass());

  dfpa::DFPAConfig Config;
  Config.indirect_ctx_k = DFPAIndirectCtxK;
  Config.max_demand_states = DFPAMaxDemandStates;
  auto *Pass = new dfpa::DFPAPass(Config);
  PM.add(Pass);
  PM.run(M);

  processDirectCalls(M, CG);

  for (const auto &Entry : Pass->getResult().getAllTargets()) {
    auto *CB = const_cast<llvm::CallBase *>(Entry.first);
    if (!CB || !CB->getFunction() || CB->getFunction()->isDeclaration())
      continue;
    for (auto *Callee : Entry.second.targets)
      addCallEdge(CG, CB->getFunction(), CB, Callee);
  }
}

int main(int Argc, char *Argv[]) {
  llvm::InitLLVM X(Argc, Argv);
  cl::HideUnrelatedOptions(CGCat);
  cl::ParseCommandLineOptions(Argc, Argv, "Call Graph Construction Tool\n");

  DiagTimer LoadingTm("Loading IR");
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Err;
  auto M = lotus::alias::tools::loadIRModule(IRFile, Context, Err, Argv[0]);
  if (!M) {
    return 1;
  }
  llvm::errs() << "Loaded IR (" << LoadingTm.elapsed() << "s)\n";

  llvm::CallGraph CG(*M);
  {
    DiagTimer Tm("Building call graph");
    switch (AnalysisType) {
    case CGType::DyckAA:
      buildCGWithDyckAA(*M, CG);
      break;
    case CGType::LotusAA:
      buildCGWithLotusAA(*M, CG);
      break;
    case CGType::DFPA:
      buildCGWithDFPA(*M, CG);
      break;
    case CGType::AserPTA_CI:
    case CGType::AserPTA_1CFA:
    case CGType::AserPTA_2CFA:
      buildCGWithAserPTA(*M, CG, AnalysisType);
      break;
    default:
      buildCGWithFPA(*M, CG, AnalysisType);
      break;
    }
  }

  llvm::Optional<llvm::raw_fd_ostream> OS;
  auto GetOS = [&]() -> llvm::raw_ostream & {
    if (!OS) {
      std::error_code EC;
      OS.emplace(OutputFile, EC);
      if (EC) {
        llvm::WithColor::error()
            << "Could not open output-file: " << EC.message() << '\n';
        std::exit(1);
      }
    }
    return *OS;
  };

  if (EmitCGAsDot)
    printCGAsDot(CG, GetOS());
  if (EmitCGAsJson)
    printCGAsJson(CG, GetOS());
  if (EmitStats)
    computeCGStats(CG, GetOS());

  return 0;
}

// Statistics and output functions
static constexpr unsigned Indent = 48;

template <typename T>
void printAlign(llvm::raw_ostream &OS, llvm::StringRef Label, T Value) {
  OS << Label;
  for (size_t i = Label.size(); i < Indent; ++i)
    OS << " ";
  OS << Value << "\n";
}

static void computeCGStats(llvm::CallGraph &CG, llvm::raw_ostream &OS) {
  llvm::Module &M = CG.getModule();
  std::map<llvm::CallBase *, std::pair<size_t, bool>> CallSiteInfo;

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    if (auto *Node = CG[&F]) {
      for (auto &Record : *Node) {
        if (auto *CS = dyn_cast<llvm::CallBase>(Record.first.getValue())) {
          if (CallSiteInfo.find(CS) == CallSiteInfo.end()) {
            CallSiteInfo[CS] = std::make_pair(
                0, !llvm::isa<llvm::Function>(
                       CS->getCalledOperand()->stripPointerCastsAndAliases()));
          }
          CallSiteInfo[CS].first++;
        }
      }
    }
  }

  size_t NumVtxFuns = 0, NumVtxCS = 0, NumIndCalls = 0, NumCallEdges = 0,
         NumIndCallEdges = 0;
  size_t LargestFanOut = 0;
  std::vector<uint32_t> NumCallEdgesPerCS, NumCallEdgesPerIndCS;
  size_t Counts[9] = {0}; // 0, 1, 2, >2, >5, >10, >20, >50, >100

  for (auto &F : M) {
    if (!F.isDeclaration())
      NumVtxFuns++;
  }

  for (const auto &Pair : CallSiteInfo) {
    size_t NumCallees = Pair.second.first;
    bool IsInd = Pair.second.second;
    NumVtxCS++;
    NumIndCalls += IsInd;
    NumCallEdges += NumCallees;
    NumIndCallEdges += NumCallees * IsInd;
    NumCallEdgesPerCS.push_back(NumCallees);
    if (IsInd)
      NumCallEdgesPerIndCS.push_back(NumCallees);
    if (NumCallees > LargestFanOut)
      LargestFanOut = NumCallees;
    if (IsInd) {
      Counts[0] += (NumCallees == 0);
      Counts[1] += (NumCallees == 1);
      Counts[2] += (NumCallees == 2);
      Counts[3] += (NumCallees > 2);
      Counts[4] += (NumCallees > 5);
      Counts[5] += (NumCallees > 10);
      Counts[6] += (NumCallees > 20);
      Counts[7] += (NumCallees > 50);
      Counts[8] += (NumCallees > 100);
    }
  }

  llvm::sort(NumCallEdgesPerCS);
  llvm::sort(NumCallEdgesPerIndCS);

  OS << "================== CallGraph Statistics ==================\n";
  printAlign(OS, "Num vertex functions", NumVtxFuns);
  printAlign(OS, "Num call-sites", NumVtxCS);
  printAlign(OS, "Num call-edges", NumCallEdges);

  if (NumCallEdgesPerCS.empty()) {
    printAlign(OS, "Avg num call-edges per call-site", "<none>");
    printAlign(OS, "Med num call-edges per call-site", "<none>");
    printAlign(OS, "90% num call-edges per call-site", "<none>");
  } else {
    printAlign(OS, "Avg num call-edges per call-site",
               double(NumCallEdges) / NumVtxCS);
    printAlign(OS, "Med num call-edges per call-site",
               NumCallEdgesPerCS[NumCallEdgesPerCS.size() / 2]);
    printAlign(OS, "90% num call-edges per call-site",
               NumCallEdgesPerCS[size_t(NumCallEdgesPerCS.size() * 0.9)]);
  }

  OS << '\n';
  printAlign(OS, "Num indirect call-sites", NumIndCalls);
  printAlign(OS, "Num indirect call-edges", NumIndCallEdges);

  if (NumCallEdgesPerIndCS.empty()) {
    printAlign(OS, "Avg num call-edges per indirect call-site", "<none>");
    printAlign(OS, "Med num call-edges per indirect call-site", "<none>");
    printAlign(OS, "90% num call-edges per indirect call-site", "<none>");
  } else {
    printAlign(OS, "Avg num call-edges per indirect call-site",
               double(NumIndCallEdges) / NumIndCalls);
    printAlign(OS, "Med num call-edges per indirect call-site",
               NumCallEdgesPerIndCS[NumCallEdgesPerIndCS.size() / 2]);
    printAlign(OS, "90% num call-edges per indirect call-site",
               NumCallEdgesPerIndCS[size_t(NumCallEdgesPerIndCS.size() * 0.9)]);
  }

  printAlign(OS, "Largest fanout (max num callees per call-site)",
             LargestFanOut);
  OS << '\n';
  printAlign(OS, "Num indirect calls with 0 resolved callees", Counts[0]);
  printAlign(OS, "Num indirect calls with 1 resolved callee", Counts[1]);
  printAlign(OS, "Num indirect calls with 2 resolved callees", Counts[2]);
  printAlign(OS, "Num indirect calls with >  2 resolved callees", Counts[3]);
  printAlign(OS, "Num indirect calls with >  5 resolved callees", Counts[4]);
  printAlign(OS, "Num indirect calls with > 10 resolved callees", Counts[5]);
  printAlign(OS, "Num indirect calls with > 20 resolved callees", Counts[6]);
  printAlign(OS, "Num indirect calls with > 50 resolved callees", Counts[7]);
  printAlign(OS, "Num indirect calls with >100 resolved callees", Counts[8]);
}

static void printCGAsDot(llvm::CallGraph &CG, llvm::raw_ostream &OS) {
  OS << "digraph \"CallGraph\" {\n  label=\"Call Graph\";\n  labelloc=top;\n  "
        "rankdir=TB;\n\n";
  llvm::Module &M = CG.getModule();

  for (auto &F : M) {
    if (!F.isDeclaration()) {
      OS << "  \"" << F.getName() << "\" [shape=record];\n";
    }
  }

  OS << "\n";
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    if (auto *Node = CG[&F]) {
      for (auto &Record : *Node) {
        if (auto *CS = dyn_cast<llvm::CallBase>(Record.first.getValue())) {
          if (auto *CalleeNode = Record.second) {
            if (auto *Callee = CalleeNode->getFunction()) {
              OS << "  \"" << F.getName() << "\" -> \"" << Callee->getName()
                 << "\";\n";
            }
          }
        }
      }
    }
  }
  OS << "}\n";
}

static void printCGAsJson(llvm::CallGraph &CG, llvm::raw_ostream &OS) {
  llvm::Module &M = CG.getModule();
  OS << "{\n  \"callgraph\": {\n    \"nodes\": [\n";

  bool First = true;
  for (auto &F : M) {
    if (!F.isDeclaration()) {
      if (!First)
        OS << ",\n";
      First = false;
      OS << "      { \"name\": \"" << F.getName() << "\" }";
    }
  }

  OS << "\n    ],\n    \"edges\": [\n";
  First = true;
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    if (auto *Node = CG[&F]) {
      for (auto &Record : *Node) {
        if (auto *CS = dyn_cast<llvm::CallBase>(Record.first.getValue())) {
          if (auto *CalleeNode = Record.second) {
            if (auto *Callee = CalleeNode->getFunction()) {
              if (!First)
                OS << ",\n";
              First = false;
              OS << "      { \"caller\": \"" << F.getName()
                 << "\", \"callee\": \"" << Callee->getName() << "\" }";
            }
          }
        }
      }
    }
  }
  OS << "\n    ]\n  }\n}\n";
}
