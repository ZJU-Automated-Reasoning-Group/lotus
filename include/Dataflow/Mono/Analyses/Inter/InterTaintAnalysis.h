#ifndef LOTUS_DATAFLOW_MONO_ANALYSES_INTER_INTERTAINTANALYSIS_H_
#define LOTUS_DATAFLOW_MONO_ANALYSES_INTER_INTERTAINTANALYSIS_H_

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/Core/CallStringSolver.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace mono {

struct InterMonoTaintConfig {
  std::unordered_set<std::string> SourceFunctions;
  std::unordered_set<std::string> SinkFunctions;
  bool TaintPointerArgsFromSources = true;
};

struct InterMonoTaintReport {
  std::map<llvm::Instruction *, std::set<llvm::Value *>> Leaks;
};

constexpr unsigned kDefaultTaintCallStringLength = 2;
using InterMonoTaintResult =
    dataflow::ContextSensitiveDataFlowResult<kDefaultTaintCallStringLength,
                                             SetContainer<llvm::Value *>>;

struct InterMonoTaintAnalysisResult {
  std::unique_ptr<InterMonoTaintResult> Results;
  InterMonoTaintReport Report;
};

// Interprocedural taint analysis (call-string length is fixed at 2).
InterMonoTaintAnalysisResult
runInterMonoTaintAnalysis(llvm::Function *Entry,
                          const InterMonoTaintConfig &Config);

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_ANALYSES_INTER_INTERTAINTANALYSIS_H_
