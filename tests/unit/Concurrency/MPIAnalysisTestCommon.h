#pragma once

#include "Analysis/Concurrency/MPI/MPIAnalysis.h"

#include "Analysis/Concurrency/MPI/MPISemantics.h"

#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

using namespace llvm;
using namespace mpi;

class MPIAnalysisTest : public lotus::unittest::LlvmModuleTest {};

static const MPIOperation *findOperation(const std::vector<MPIOperation> &ops,
                                         ThreadAPI::TD_TYPE type) {
  for (const auto &op : ops) {
    if (op.td_type == type) {
      return &op;
    }
  }
  return nullptr;
}
