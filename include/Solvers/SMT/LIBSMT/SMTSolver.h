#pragma once

#include "Solvers/SMT/LIBSMT/SMTObject.h"
#include "z3++.h"

#include <vector>

class SMTFactory;
class SMTModel;
class SMTExpr;
class SMTExprVec;
class MessageQueue;

class SMTSolver : public SMTObject {
public:
  enum SMTResultType { SMTRT_Unsat, SMTRT_Sat, SMTRT_Unknown, SMTRT_Uncheck };

  static int GlobalTimeout;

private:
  z3::solver Solver;

  // z3::model ModelCache;
  // heuristic: as solver is called incrementally, the initial queries are small
  // and the ModelCache may fail
  unsigned checkCount;
  // unsigned missCount;

  SMTSolver(SMTFactory *F, z3::solver &Z3Solver, z3::model &Z3Model);

public:
  virtual ~SMTSolver();

  SMTSolver(const SMTSolver &Solver);

  SMTSolver &operator=(const SMTSolver &Solver);

  void add(SMTExpr);

  void addAll(SMTExprVec);

  void addAll(const std::vector<SMTExpr> &EVec);

  // set a specific Timeout for current query.
  // in some cases, we may want to spent more/less time than the default timeout
  // on a query
  virtual SMTResultType check(unsigned Timeout = 0);

  // check sat/unsat with fast, incomplete theory-level decision procedures
  virtual SMTResultType fastCheck(unsigned Timeout = 1000);

  // Different procedures for solving an N2N query(Given P1,P2,...,PN. We want
  // to get their satisfiability results respectively.)
  /*
   *   - By default, we usually call the solver N times("check-one-by-one"),
   * which is not listed below
   *   - checkN2NQueryWithUnderAppro(): merge with logical and; check the merged
   * constraint.
   *   - CheckN2NQueryWithOverAppro(): merge with logical or; apply model-based
   * refinement(an Abstraction-Refinement loop)
   */

  // interface function. return true if success; else return false, and you may
  // have to call "check-one-by-one")
  virtual bool
  checkN2NQueryWithUnderAppro(SMTExprVec &FVec,
                              std::vector<SMTSolver::SMTResultType> &ResVec);

  // helper function. return true if all success; else return false(some are
  // known, some are not)..
  virtual bool
  overApproCheckMisc(SMTExpr &G, SMTExprVec &FVec,
                     std::vector<SMTSolver::SMTResultType> &Labels);

  // interface function. return true if all success; else return false(some are
  // known, some are not)..
  virtual bool
  checkN2NQueryWithOverAppro(SMTExprVec &FVec,
                             std::vector<SMTSolver::SMTResultType> &ResVec);

  // instead of using the global timeout option, setting timeout explicitly for
  // a SMTSolver
  void setTimeout(unsigned Timeout);

  SMTModel getSMTModel();

  SMTExprVec assertions();

  virtual void reset();

  virtual void push();

  virtual void pop(unsigned N = 1);

  unsigned getNumScopes();

  bool operator<(const SMTSolver &Solver) const;

  // Reset the global timeout if no special user specification
  static void setDefaultGlobalTimeout(int NewTimeout);

  friend std::ostream &operator<<(std::ostream &O, SMTSolver &Solver) {
    O << Solver.Solver.to_smt2() << "\n";
    return O;
  }

  friend class SMTModel;
  friend class SMTFactory;
};
