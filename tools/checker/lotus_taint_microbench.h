#pragma once

#include <string>

#include <llvm/Support/raw_ostream.h>

namespace ifds {
class TaintAnalysis;
template <typename> class IFDSSolver;
} // namespace ifds

void runMicroBenchEvaluation(
    const ifds::TaintAnalysis &analysis,
    const ifds::IFDSSolver<ifds::TaintAnalysis> &solver,
    const std::string &expected_path, bool verbose, llvm::raw_ostream &os);
