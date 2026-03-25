#pragma once

#include "Solvers/SMT/LIBSMT/SMTExpr.h"
#include "Solvers/SMT/LIBSMT/SMTSolver.h"
#include "z3++.h"

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class SMTRenamingAdvisor {
public:
  virtual ~SMTRenamingAdvisor() {}

  /// It determines if an expr will be pruned when renaming
  /// See SMTFactory::rename for details
  virtual bool prune(const SMTExpr &) = 0;

  /// It determines if an expr will be renamed
  /// See SMTFactory::rename for details
  virtual bool rename(const SMTExpr &) = 0;
};

/// When a SMTFactory is deconstructed, the SMTExpr, SMTExprVec,
/// SMTModel, SMTSolver created by the factory become invalid.
/// Thus, please deconstruct them before deconstructing
/// the factory.
///
/// Constraints built by the same SMTFactory instance cannot be
/// accessed concurrently. SMTFactory provides a FactoryLock
/// for concurrency issues.
class SMTFactory {
private:
  z3::context Ctx;

  std::mutex FactoryLock;

  // std::string Tactic;

  unsigned TempSMTVaraibleIndex;

public:
  SMTFactory();

  ~SMTFactory() {}

  SMTSolver createSMTSolver();

  SMTSolver createSMTSolverWithTactic(const std::string &Tactic = "smt");

  SMTExpr createEmptySMTExpr();

  SMTExprVec createEmptySMTExprVec();

  SMTExprVec createBoolSMTExprVec(bool Val, size_t Sz);

  SMTExprVec createSMTExprVec(const std::vector<SMTExpr> &ExprVec);

  SMTExpr createRealConst(const std::string &Name);

  SMTExpr createRealVal(const std::string &ValStr);

  SMTExpr createBitVecConst(const std::string &Name, uint64_t Sz);

  SMTExpr createBoolConst(const std::string &Name);

  SMTExpr createBoolVal(bool Val);

  SMTExpr createTemporaryBitVecConst(uint64_t Sz);

  SMTExpr createBitVecVal(const std::string &ValStr, uint64_t Sz);

  SMTExpr createBitVecVal(uint64_t Val, uint64_t Sz);

  SMTExpr createIntVal(int Val);

  SMTExpr createSelect(SMTExpr &Vec, SMTExpr Index);

  SMTExpr createStore(SMTExpr &Vec, SMTExpr Index, SMTExpr &Val2Store);

  SMTExpr createIntRealArrayConstFromStringSymbol(const std::string &Name);

  SMTExpr createIntBvArrayConstFromStringSymbol(const std::string &Name,
                                                uint64_t Sz);

  SMTExpr createIntDomainConstantArray(SMTExpr &ElmtExpr);

  /// This function translate an SMTExprVec/SMTExpr (the 1st parameter) created
  /// by other SMTFactory to the context of this SMTFactory.
  SMTExprVec translate(const SMTExprVec &);
  SMTExpr translate(const SMTExpr &Expr);

  /// The variables in the constraints will be renamed using a suffix
  /// (the 2nd parameter).
  /// Since the symbols of the variables are renamed, we record the
  /// <old symbol, new expr> map in the 3rd parameter.
  ///
  /// The last parameter is optional, which indicates when an expr should
  /// be pruned in the translated constraints. For examples, the original
  /// constraint is as following:
  ///                 "x + y > z && (a || b) && !c",
  /// and suppose that "x" is the variable be pruned (in other words, we
  /// do not want to see the variable "x" in the translated constraint).
  /// In this example, "x + y > z" will be replaced by "true".
  ///
  /// This function returns a std::pair, in which the first is the translated
  /// constraint, and the second indicates if some variables are pruned.
  std::pair<SMTExprVec, bool> rename(const SMTExprVec &, const std::string &,
                                     std::unordered_map<std::string, SMTExpr> &,
                                     SMTRenamingAdvisor * = nullptr);

  std::mutex &getFactoryLock() { return FactoryLock; }

  z3::context &getZ3Ctx() { return Ctx; }

  SMTExpr createSMTExprFromStream(std::string &ExprStr);

  SMTExpr parseSMTLib2String(const std::string &);

  SMTExpr parseSMTLib2File(const std::string &);

private:
  typedef struct RenamingUtility {
    bool WillBePruned;
    SMTExpr AfterBeingPruned;
    SMTExprVec ToPrune;
    std::unordered_map<std::string, SMTExpr> SymbolMapping;
  } RenamingUtility;

  std::map<SMTExpr, RenamingUtility, SMTExprComparator> ExprRenamingCache;

  /// Utility for public function translate
  /// It visits all exprs in a ``big" expr.
  bool visit(SMTExpr &, std::unordered_map<std::string, SMTExpr> &,
             SMTExprVec &, std::map<SMTExpr, bool, SMTExprComparator> &,
             SMTRenamingAdvisor *);
};