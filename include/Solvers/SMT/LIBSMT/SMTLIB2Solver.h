#ifndef SMTLIB2_SOLVER_H
#define SMTLIB2_SOLVER_H

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <memory>
#include <system_error>
#include <vector>

typedef std::function<int(
    const std::vector<std::string> &Args, llvm::StringRef RedirectIn,
    llvm::StringRef RedirectOut, llvm::StringRef RedirectErr, unsigned Timeout)>
    SolverProgram;

class SMTLIBSolver {
public:
  virtual ~SMTLIBSolver();
  virtual std::string getName() const = 0;
  virtual std::error_code isSatisfiable(llvm::StringRef Query, bool &Result,
                                        unsigned NumModels,
                                        std::vector<llvm::APInt> *Models,
                                        unsigned Timeout = 0) = 0;
};

SolverProgram makeExternalSolverProgram(llvm::StringRef Path);
SolverProgram makeInternalSolverProgram(int MainPtr(int argc, char **argv));

std::unique_ptr<SMTLIBSolver> createZ3Solver(SolverProgram Prog, bool Keep);

#endif // SMTLIB2_SOLVER_H