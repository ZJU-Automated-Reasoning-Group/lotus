#pragma once

#include "Analysis/Concurrency/MPI/MPIAnalysis.h"
#include "Checker/Concurrency/ConcurrencyBugReport.h"

#include <memory>
#include <vector>

namespace concurrency {

class MPIChecker {
public:
  MPIChecker(llvm::Module &module, mpi::MPIAnalysis *analysis = nullptr);

  std::vector<ConcurrencyBugReport> checkMPIBugs();

private:
  llvm::Module &m_module;
  mpi::MPIAnalysis *m_analysis;
  std::unique_ptr<mpi::MPIAnalysis> m_ownedAnalysis;

  void ensureAnalysis();
};

} // namespace concurrency