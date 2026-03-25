#include "Fuzzing/AFLGoLinker/DAFL.h"
#include "Fuzzing/Analysis/DAFL.h"

#include <llvm/IR/Attributes.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

const char *AFLGoTraceBBDAFL = "__aflgo_trace_bb_dafl";

PreservedAnalyses DAFLInstrumentationPass::run(Module &M,
                                               ModuleAnalysisManager &AM) {

  auto &Scores = AM.getResult<DAFLAnalysis>(M);

  if (!Scores.has_value()) {
    return PreservedAnalyses::all();
  }

  if (Scores->empty()) {
    errs() << "[DAFL] empty scores map\n";
    return PreservedAnalyses::all();
  }

  std::unique_ptr<raw_fd_ostream> Out = nullptr;

  if (OutputFile == "-") {
    Out = std::make_unique<raw_fd_ostream>(sys::fs::getStderrHandle(), false);

  } else if (!OutputFile.empty()) {
    int FD;
    auto EC = sys::fs::openFileForWrite(
        OutputFile, FD, sys::fs::CD_CreateAlways, sys::fs::OF_Text);

    errs() << "[DAFL] output file: " << OutputFile << "\n";

    if (EC == std::errc::file_exists) {
      errs() << "[DAFL] file exists, overwriting\n";
    } else if (EC) {
      auto Err =
          formatv("error opening file {0}: {1}", OutputFile, EC.message());
      report_fatal_error(Err);
    }

    Out = std::make_unique<raw_fd_ostream>(FD, true);
  }

  auto &C = M.getContext();
  auto *VoidTy = Type::getVoidTy(C);
  auto *Int64Ty = Type::getInt64Ty(C);
  FunctionType *FT = FunctionType::get(VoidTy, {Int64Ty}, false);
  auto Fn = M.getOrInsertFunction(AFLGoTraceBBDAFL, FT);

  for (auto &F : M) {
    auto IsFnReachable = false;
    for (auto &BB : F) {
      auto Score = Scores->find(&BB);
      if (Score == Scores->end()) {
        continue;
      }

      IsFnReachable = true;

      auto *ScoreValue = ConstantInt::get(Int64Ty, Score->second);
      IRBuilder<> IRB(&*BB.getFirstInsertionPt());
      IRB.CreateCall(Fn, {ScoreValue});

      if (Out) {
        DILocation *Loc = nullptr;
        for (auto &I : BB) {
          Loc = I.getDebugLoc().get();
          if (Loc && !Loc->getFilename().empty() && Loc->getLine() > 0) {
            break;
          }
          Loc = nullptr;
        }

        if (!Loc) {
          errs() << "[DAFL] no debug info for BB " << BB.getName()
                 << " in function " << F.getName() << "\n";
          continue;
        }

        auto AbsolutePath = SmallString<128>(Loc->getFilename());
        sys::fs::make_absolute(Loc->getDirectory(), AbsolutePath);
        auto RealPath = SmallString<128>();
        sys::fs::real_path(AbsolutePath, RealPath);

        *Out << formatv("{0},{1},{2}:{3}\n", Score->second, F.getName(),
                        RealPath, Loc->getLine());
      }
    }

    if (!IsFnReachable) {
      F.addFnAttr(Attribute::NoSanitizeCoverage);
    }
  }

  PreservedAnalyses PA;
  PA.preserve<DAFLAnalysis>();
  PA.preserve<AFLGoTargetDetectionAnalysis>();
  return PA;
}