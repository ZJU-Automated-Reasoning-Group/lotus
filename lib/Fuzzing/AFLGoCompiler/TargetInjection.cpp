#include "Fuzzing/AFLGoCompiler/TargetInjection.h"
#include "Fuzzing/Analysis/TargetDetection.h"

#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/VirtualFileSystem.h>

using namespace llvm;

cl::opt<std::string>
    TargetsPath("targets",
                cl::desc("Input file containing the target lines of code."),
                cl::value_desc("targets"));

cl::opt<bool>
    SkipRealPath("skip-real-path",
                 cl::desc("Skip real path resolution for target lines."),
                 cl::init(false));

cl::opt<bool>
    NoTargetsNoError("targets-no-error",
                     cl::desc("Don't error out if targets are not found."),
                     cl::init(false));

bool AFLGoTargetInjectionPass::Target::matches(const DILocation &Loc) {
  auto Line = Loc.getLine();
  auto File = Loc.getFilename();
  auto Directory = Loc.getDirectory();

  if (File.empty()) {
    if (auto *OriginalLoc = Loc.getInlinedAt()) {
      Line = OriginalLoc->getLine();
      File = OriginalLoc->getFilename();
      Directory = OriginalLoc->getDirectory();
    }
  }

  auto AbsolutePath = SmallString<128>(File);
  sys::fs::make_absolute(Directory, AbsolutePath);
  StringRef FinalPath;
  if (SkipRealPath) {
    FinalPath = AbsolutePath;
  } else {
    auto RealPath = SmallString<128>();
    sys::fs::real_path(AbsolutePath, RealPath);
    FinalPath = RealPath;
  }

  return !FinalPath.compare(this->File) && Line == this->Line;
}

void AFLGoTargetInjectionPass::parseTargets(
    std::unique_ptr<MemoryBuffer> &TargetsBuffer) {
  SmallVector<StringRef, 16> Lines;
  TargetsBuffer->getBuffer().split(Lines, '\n');
  for (auto Line : Lines) {
    if (Line.empty() || Line[0] == '#') {
      continue;
    }

    auto LineSplit = Line.split(':');
    auto File = LineSplit.first;
    auto LineNumStr = LineSplit.second;
    auto LineNum = std::stoul(LineNumStr.str());
    Targets.push_back(Target(File.str(), LineNum));
  }
}

AFLGoTargetInjectionPass::AFLGoTargetInjectionPass() {
  auto VFS = vfs::getRealFileSystem();
  auto BufferOrErr = VFS->getBufferForFile(TargetsPath);
  if (std::error_code EC = BufferOrErr.getError()) {
    std::string ErrorMessage = formatv("can't open targets file '{0}': {1}",
                                       TargetsPath, EC.message());
    report_fatal_error(Twine(ErrorMessage));
  }

  parseTargets(*BufferOrErr);
}

PreservedAnalyses AFLGoTargetInjectionPass::run(Module &M,
                                                ModuleAnalysisManager &MAM) {
  auto &C = M.getContext();
  auto *VoidTy = Type::getVoidTy(C);
  auto *Int32Ty = Type::getInt32Ty(C);
  FunctionType *FT = FunctionType::get(VoidTy, {Int32Ty}, false);
  auto AFLGoTraceBBTarget = M.getOrInsertFunction(
      AFLGoTargetDetectionAnalysis::TargetFunctionName, FT);

  SmallSetVector<Target *, 16> Seen;

  for (auto &F : M) {
    if (F.isDeclaration()) {
      continue;
    }

    for (auto &BB : F) {
      bool BBIsTarget = false;
      bool BBHasTarget = false;

      for (auto &I : BB) {
        if (auto const *CI = dyn_cast<CallInst>(&I)) {
          if (CI->getCalledFunction() == AFLGoTraceBBTarget.getCallee()) {
            BBHasTarget = true;
          }
        }

        auto *Loc = I.getDebugLoc().get();
        if (!Loc) {
          continue;
        }

        for (auto &Target : Targets) {
          if (Target.matches(*Loc)) {
            Seen.insert(&Target);
            BBIsTarget = true;
            I.addAnnotationMetadata(
                AFLGoTargetDetectionAnalysis::TargetInstructionAnnotation);
            break;
          }
        }
      }

      if (!BBHasTarget && BBIsTarget) {
        IRBuilder<> IRB(&*BB.getFirstInsertionPt());
        auto *TargetIDPlaceholder = IRB.getInt32(0);
        IRB.CreateCall(AFLGoTraceBBTarget, {TargetIDPlaceholder});
      }
    }
  }

  for (auto &Target : Targets) {
    if (!Seen.count(&Target)) {
      errs() << "Target not found: " << Target << '\n';
    }
  }
  if (!NoTargetsNoError && Targets.size() != Seen.size()) {
    report_fatal_error("Not all targets were found.");
  }

  return PreservedAnalyses::none();
}