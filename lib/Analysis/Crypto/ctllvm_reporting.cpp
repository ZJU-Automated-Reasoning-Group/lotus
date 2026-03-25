/*
 * Reporting and statistics helpers for the ctllvm pass.
 */

#include "CTInternal.h"

#include "llvm/Support/raw_ostream.h"

#include <fstream>

using namespace llvm;

namespace ctllvm {
namespace detail {

int CryptoAnalysisImpl::checkAndReport(
    Value *Arg, Function &F, FunctionAnalysisManager &FAM,
    SetVector<Instruction *> &taintedInstructions,
    std::map<Instruction *, int> &leak_through_cache,
    std::map<Instruction *, int> &leak_through_branch,
    std::map<Instruction *, int> &leak_through_variable_timing, int mode) {
  checkInstructionLeaks(taintedInstructions, leak_through_cache,
                        leak_through_branch, leak_through_variable_timing, Arg,
                        F, FAM);

  int tainted_line = getDebugLine(Arg, "", F);
  int analyzed_lines = taintedInstructions.size();
  StringRef tested_value_name = getDebugName(Arg, "", F);
  StringRef file_name = "";
  if (DISubprogram *SP = F.getSubprogram()) {
    file_name = SP->getFilename();
  } else {
    file_name = F.getParent()->getSourceFileName();
  }

  if (options_.try_hard_on_name && tainted_line == -1 &&
      tested_value_name.empty()) {
    for (auto *U : Arg->users()) {
      if (auto *SI = dyn_cast<StoreInst>(U)) {
        Value *store_address = SI->getPointerOperand();
        tainted_line = getDebugLine(store_address, "", F);
        tested_value_name = getDebugName(store_address, "", F);
        break;
      }
    }
  }

  StringRef function_name = stripClonedSuffix(F.getName());
  errs() << "{"
         << "\"function\": \"" << function_name << "\", "
         << "\"file\": \"" << file_name << "\", "
         << "\"tested_value\": \"" << tested_value_name << "\", "
         << "\"line\": \"" << tainted_line << "\", "
         << "\"IR\": \"" << *Arg << "\", "
         << "\"LoCs\": " << analyzed_lines << ", "
         << "\"Feature\": " << mode << ", "
         << "\"cache\": " << leak_through_cache.size() << ", "
         << "\"branch\": " << leak_through_branch.size() << ", "
         << "\"vt\": " << leak_through_variable_timing.size() << "}\n";
  if (options_.report_leakages) {
    reportLeakage(taintedInstructions, leak_through_cache, leak_through_branch,
                  leak_through_variable_timing, mode);
  }
  return leak_through_cache.size() + leak_through_branch.size() +
         leak_through_variable_timing.size();
}

void CryptoAnalysisImpl::printLeakage(
    const std::string &type, const std::map<Instruction *, int> &leakMap,
    int may_must, SetVector<Instruction *> &taintedInstructions) {
  (void)taintedInstructions;

  for (const auto &it : leakMap) {
    StringRef filename = "unknown";
    std::string local_type = type;
    if (isa<SelectInst>(it.first)) {
      local_type = "select";
    }

    if (it.second != -1) {
      filename = it.first->getModule()->getSourceFileName();
      if (auto dbgLoc = it.first->getDebugLoc()) {
        auto *scope = dbgLoc->getScope();
        if (scope) {
          filename = scope->getFilename();
        }
      }
    }

    if (options_.debug) {
      if (may_must != 2) {
        errs() << local_type << " violate CT policy at: " << *it.first << " in "
               << options_.file_path + filename.str() << " at line "
               << it.second << "\n";
      } else {
        errs() << "May " << local_type << " violate CT policy at: " << *it.first
               << " in " << options_.file_path + filename.str() << " at line "
               << it.second << "\n";
      }
    } else if (may_must != 2) {
      errs() << "  Violate CT policy: " << local_type << " in file "
             << options_.file_path + filename.str() << " at line " << it.second
             << "\n";
    } else {
      errs() << "  May Violate CT policy: " << local_type << " in file "
             << options_.file_path + filename.str() << " at line " << it.second
             << "\n";
    }

    if (it.second != -1) {
      printSourceCode(filename.str(), it.second);
    }
  }
}

void CryptoAnalysisImpl::reportLeakage(
    SetVector<Instruction *> &taintedInstructions,
    std::map<Instruction *, int> &leak_through_cache,
    std::map<Instruction *, int> &leak_through_branch,
    std::map<Instruction *, int> &leak_through_variable_timing, int may_must) {
  bool has_leakage =
      (!leak_through_cache.empty() || !leak_through_branch.empty() ||
       !leak_through_variable_timing.empty());
  if (!has_leakage) {
    return;
  }

  printLeakage("cache", leak_through_cache, may_must, taintedInstructions);
  printLeakage("branch", leak_through_branch, may_must, taintedInstructions);
  printLeakage("variable timing", leak_through_variable_timing, may_must,
               taintedInstructions);
}

void CryptoAnalysisImpl::printSourceCode(const std::string &filename,
                                         int line_number) {
  const std::string resolved_path = options_.file_path + filename;
  std::ifstream file(resolved_path);
  if (!file.is_open()) {
    errs() << "Cannot open file " << resolved_path << "\n";
    return;
  }

  errs().changeColor(raw_ostream::RED);
  std::string source_line;
  for (unsigned current_line = 1; std::getline(file, source_line);
       ++current_line) {
    if (static_cast<int>(current_line) == line_number) {
      errs() << "  -->" << source_line << "\n";
      break;
    }
  }
  errs().resetColor();
}

void CryptoAnalysisImpl::printStatistics() {
  int inline_fail = 0;
  int inline_itself = 0;
  int inline_assembly = 0;
  int indirect_call = 0;
  int no_implementation = 0;
  int invoke_function = 0;
  int not_callbase = 0;
  int over_threshold = 0;

  errs() << "===========REPORTING Analysis Overivew=============\n";
  errs() << "Number of overall functions: " << statistics_.overall_functions
         << "\n";
  errs() << "Number of analyzed functions: " << statistics_.analyzed_functions
         << "\n";
  errs() << "Number of no constant size memcpy: " << statistics_.no_constant_size
         << "\n";
  errs() << "Number of too many alias: " << statistics_.too_many_alias << "\n";
  errs() << "Number of secure functions: " << statistics_.secure_functions
         << "\n";
  errs() << "Number of analyzed taint sources: " << statistics_.taint_source
         << "\n";
  errs() << "Number of secure taint sources: "
         << statistics_.secure_taint_source << "\n";
  errs() << "==================================================\n";

  if (options_.soundness_mode) {
    for (AnalysisError code : statistics_.cannot_inline_cases) {
      switch (code) {
      case AnalysisError::InlineFail:
        inline_fail++;
        break;
      case AnalysisError::InlineItself:
        inline_itself++;
        break;
      case AnalysisError::InlineAssembly:
        inline_assembly++;
        break;
      case AnalysisError::IndirectCall:
        indirect_call++;
        break;
      case AnalysisError::NoImplementation:
        no_implementation++;
        break;
      case AnalysisError::InvokeFunction:
        invoke_function++;
        break;
      case AnalysisError::NotCallBase:
        not_callbase++;
        break;
      case AnalysisError::OverThreshold:
        over_threshold++;
        break;
      default:
        break;
      }
    }

    errs() << "===========REPORTING INLINE STATISTIC=============\n";
    errs() << "Number of Success inline: " << statistics_.inline_success << "\n";
    errs() << "Number of Over Threshold: " << over_threshold << "\n";
    errs() << "Number of inline fail: " << inline_fail << "\n";
    errs() << "Number of inline itself: " << inline_itself << "\n";
    errs() << "Number of inline assembly: " << inline_assembly << "\n";
    errs() << "Number of indirect call: " << indirect_call << "\n";
    errs() << "Number of no implementation: " << no_implementation << "\n";
    errs() << "Number of invoke function: " << invoke_function << "\n";
    errs() << "Number of not callbase: " << not_callbase << "\n";
    errs() << "==================================================\n";
  }
}

} // namespace detail
} // namespace ctllvm
