//
// Created by prophe cheng on 2024/6/25.
//
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"

#include "Alias/AliasAnalysisWrapper/CLIUtils.h"
#include "Alias/FPA/Config.h"
#include "Alias/FPA/FLTAPass.h"
#include "Alias/FPA/KELPPass.h"
#include "Alias/FPA/MLTADFPass.h"
#include "Alias/FPA/MLTAPass.h"

#include <iostream>
#include <memory>
#include <string>

using namespace llvm;
using namespace std;
using namespace lotus::alias::tools;

// Command line parameters.
cl::list<string> InputFilenames(cl::Positional, cl::OneOrMore,
                                cl::desc("<input bitcode files>"));

// 默认采用FLTA
cl::opt<int>
    AnalysisType("analysis-type",
                 cl::desc("select which analysis to use: 1 --> FLTA, 2 --> "
                          "MLTA, 3 --> Data Flow Enhanced MLTA, 4 --> Kelp"),
                 cl::NotHidden, cl::init(1));

// max_type_layer
cl::opt<int> MaxTypeLayer(
    "max-type-layer",
    cl::desc("Multi-layer type analysis for refining indirect-call targets"),
    cl::NotHidden, cl::init(10));

cl::opt<bool> DebugMode("fpa-debug", cl::desc("debug mode"), cl::init(false));

// 结果保存路径
cl::opt<string>
    OutputFilePath("output-file",
                   cl::desc("Output file path, better to use absolute path"),
                   cl::init(""));

GlobalContext GlobalCtx;

// 打印结果
void PrintResults(GlobalContext *GCtx) {
  int TotalTargets = 0;
  // 计算间接调用总共调用的target function数量
  for (auto *IC : GCtx->IndirectCallInsts)
    TotalTargets += GCtx->Callees[IC].size();

  int totalsize = 0;
  OP << "\n@@ Total number of final callees: " << totalsize << ".\n";

  OP << "############## Result Statistics ##############\n";
  // cout<<"# Ave. Number of indirect-call targets: \t" << std::setprecision(5)
  // << AveIndirectTargets<<"\n";
  OP << "# Number of virtual calls: \t\t\t" << GCtx->NumVirtualCall << "\n";
  OP << "# Number of indirect calls: \t\t\t" << GCtx->IndirectCallInsts.size()
     << "\n";
  OP << "# Number of indirect calls with targets: \t"
     << GCtx->NumValidIndirectCalls << "\n";
  OP << "# Number of indirect-call targets: \t\t"
     << GCtx->NumIndirectCallTargets << "\n";
  OP << "# Number of address-taken functions: \t\t"
     << GCtx->AddressTakenFuncs.size() << "\n";
  OP << "# Number of multi-layer calls: \t\t\t" << GCtx->NumSecondLayerTypeCalls
     << "\n";
  OP << "# Number of multi-layer targets: \t\t" << GCtx->NumSecondLayerTargets
     << "\n";
  OP << "# Number of one-layer calls: \t\t\t" << GCtx->NumFirstLayerTypeCalls
     << "\n";
  OP << "# Number of one-layer targets: \t\t\t" << GCtx->NumFirstLayerTargets
     << "\n";
  OP << "# Number of simple indirect calls: \t\t\t" << GCtx->NumSimpleIndCalls
     << "\n";
  OP << "# Number of confined functions: \t\t\t" << GCtx->NumConfinedFuncs
     << "\n";

  // 根据OutputFilePath决定输出方式
  if (OutputFilePath.size() == 0)
    return;
  ostream &output =
      (OutputFilePath == "cout") ? cout : *(new std::ofstream(OutputFilePath));

  for (auto &curEle : GCtx->Callees) {
    if (curEle.first->isIndirectCall()) {
      totalsize += curEle.second.size();
      FuncSet funcs = curEle.second;

      auto *Scope = cast<DIScope>(curEle.first->getDebugLoc().getScope());
      string callsiteFile = Scope->getFilename().str();
      int line = curEle.first->getDebugLoc().getLine();
      int col = curEle.first->getDebugLoc().getCol();
      string content =
          callsiteFile + ":" + itostr(line) + ":" + itostr(col) + "|";
      for (llvm::Function *func : funcs)
        content += (func->getName().str() + ",");
      content = content.substr(0, content.size() - 1);
      content += "\n";
      output << content;
    }
  }

  // 如果是文件输出，需要关闭并释放资源
  if (OutputFilePath.size() != 0) {
    static_cast<std::ofstream &>(output).close();
    delete &output;
  }
}

int main(int argc, char **argv) {
  // Print a stack trace if we signal out.
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram X(argc, argv);

  llvm_shutdown_obj Y; // Call llvm_shutdown() on exit.

  cl::ParseCommandLineOptions(argc, argv, "global analysis\n");
  SMDiagnostic Err;

  for (unsigned i = 0; i < InputFilenames.size(); ++i) {
    LLVMContext *LLVMCtx = new LLVMContext();
    auto M = loadIRModule(InputFilenames[i], *LLVMCtx, Err, argv[0]);

    if (M == nullptr) {
      OP << argv[0] << ": error loading file '" << InputFilenames[i] << "'\n";
      continue;
    }

    Module *Module = M.release();
    StringRef MName = StringRef(strdup(InputFilenames[i].data()));
    GlobalCtx.Modules.push_back(std::make_pair(Module, MName));
    GlobalCtx.ModuleMaps[Module] = InputFilenames[i];
  }

  debug_mode = DebugMode;
  max_type_layer = MaxTypeLayer;

  CallGraphPass *pass;
  // 进行indirect-call分析
  if (AnalysisType == 1)
    pass = new FLTAPass(&GlobalCtx);
  else if (AnalysisType == 2)
    pass = new MLTAPass(&GlobalCtx);
  else if (AnalysisType == 3)
    pass = new MLTADFPass(&GlobalCtx);
  else if (AnalysisType == 4)
    pass = new KELPPass(&GlobalCtx);
  else {
    cout << "unimplemnted analysis type, break\n";
    return 0;
  }
  pass->run(GlobalCtx.Modules);

  // 打印分析结果
  PrintResults(&GlobalCtx);
  return 0;
}