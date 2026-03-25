/**
 * @file SMTSolver.cpp
 * @brief Implementation of the SMTSolver class that provides the core SMT
 * solving interface
 *
 * This file implements SMTSolver, which serves as the main interface for
 * interacting with the underlying Z3 solver. The class supports:
 * - Adding and managing constraints
 * - Checking satisfiability with optional timeouts
 * - Fast approximation checking
 * - N-to-N query solving with under/over approximation
 * - Model generation for satisfiable formulas
 * - Push/pop for managing solver scopes
 *
 * The implementation uses Z3 for the actual solving, but provides a clean
 * abstraction layer for the rest of the system.
 */

#include "Solvers/SMT/LIBSMT/SMTSolver.h"

#include "Solvers/SMT/LIBSMT/SMTConfigure.h"
#include "Solvers/SMT/LIBSMT/SMTExpr.h"
#include "Solvers/SMT/LIBSMT/SMTFactory.h"
#include "Solvers/SMT/LIBSMT/SMTModel.h"

#include <fstream>
#include <iostream>
#include <map>

#include <llvm/Support/CommandLine.h>
#include <time.h>

#define DEBUG_TYPE "solver"

using namespace llvm;

static llvm::cl::opt<std::string>
    UsingSimplify("solver-simplify", llvm::cl::init(""),
                  llvm::cl::desc("Using online simplification technique. "
                                 "Candidates are local and dillig."));

static llvm::cl::opt<std::string> DumpingConstraintsDst(
    "dump-cnts-dst", llvm::cl::init(""),
    llvm::cl::desc(
        "If solving time is larger than the time that -dump-cnts-timeout, the "
        "constraints will be output the destination."));

static llvm::cl::opt<int> DumpingConstraintsTimeout(
    "dump-cnts-timeout",
    llvm::cl::desc("If solving time is too large (ms), the constraints will be "
                   "output to the destination that -dump-cnts-dst set."));

int SMTSolver::GlobalTimeout;

static llvm::cl::opt<int> EnableLocalSimplify(
    "enable-local-simplify", llvm::cl::init(0),
    llvm::cl::desc(
        "Enable local simplifications while adding a vector of constraints"));

// static llvm::cl::opt<int> EnableModelCache("enable-model-cache",
//		llvm::cl::init(0), llvm::cl::desc("Enable satisfying model
// cache"));

// only for debugging (single-thread)
bool SMTSolvingTimeOut = false;

SMTSolver::SMTSolver(SMTFactory *F, z3::solver &Z3Solver, z3::model &Z3Model)
    : SMTObject(F), Solver(Z3Solver), checkCount(0) {
  if (SMTSolver::GlobalTimeout > 0) {
    z3::params Z3Params(Z3Solver.ctx());
    Z3Params.set("timeout", (unsigned)SMTSolver::GlobalTimeout);
    Z3Solver.set(Z3Params);
  }
}

SMTSolver::SMTSolver(const SMTSolver &Solver)
    : SMTObject(Solver), Solver(Solver.Solver) {}

SMTSolver &SMTSolver::operator=(const SMTSolver &Solver) {
  SMTObject::operator=(Solver);
  if (this != &Solver) {
    this->Solver = Solver.Solver;
  }
  return *this;
}

SMTSolver::~SMTSolver() {}

SMTSolver::SMTResultType SMTSolver::check(unsigned Timeout) {
  if (Timeout > 0) {
    this->setTimeout(Timeout);
  }

  z3::check_result Result;
  try {
    clock_t Start;
    if (DumpingConstraintsTimeout.getNumOccurrences()) {
      Start = clock();
    }

    if (UsingSimplify.getNumOccurrences()) {
      z3::solver Z3Solver4Sim(Solver.ctx());

      // 1. merge as one
      SMTExpr Whole = this->assertions().toAndExpr();

      // 2. simplify
      if (UsingSimplify.getValue() == "local") {
        Z3Solver4Sim.add(Whole.localSimplify().Expr);
      } else if (UsingSimplify.getValue() == "dillig") {
        SMTExpr SimplifiedForm = Whole.dilligSimplify();
        // if (SimplifiedForm.equals(z3_solver.ctx().bool_val(false))) {
        //		return SMTResult::UNSAT;
        // } else {
        //		if (debug_bug_report) {
        //			// check so that get_model is valid
        //			z3_solver.check();
        //		}
        //		return SMTResult::SAT;
        // }
        Z3Solver4Sim.add(SimplifiedForm.Expr);
      } else {
        Z3Solver4Sim.add(Whole.Expr);
      }

      Result = Z3Solver4Sim.check();
    } else {
      Result = Solver.check();
    }

    if (DumpingConstraintsTimeout.getNumOccurrences()) {
      double TimeCost = (double)(clock() - Start) * 1000 / CLOCKS_PER_SEC;
      if (TimeCost > DumpingConstraintsTimeout.getValue() &&
          DumpingConstraintsDst.getNumOccurrences()) {
        // output the constraints to a temp file in the dst
        std::string DstFileName = DumpingConstraintsDst.getValue();
        DstFileName.append("/case");
        DstFileName.append(std::to_string(clock()));
        DstFileName.append(".smt2");

        std::ofstream DstFile;
        DstFile.open(DstFileName);

        if (DstFile.is_open()) {
          DstFile << *this << "\n";
          DstFile.close();
        } else {
          std::cerr << "File cannot be opened: " << DstFileName << "\n";
        }

        SMTSolvingTimeOut = true;
      } else {
        SMTSolvingTimeOut = false;
      }
    } else {
    }
  } catch (z3::exception &Ex) {
    std::cerr << __FILE__ << " : " << __LINE__ << " : " << Ex << "\n";
    return SMTResultType::SMTRT_Unknown;
  }

  // Use a return value to suppress gcc warning
  SMTResultType RetVal = SMTResultType::SMTRT_Unknown;

  switch (Result) {
  case z3::check_result::sat:
    RetVal = SMTResultType::SMTRT_Sat;
    break;
  case z3::check_result::unsat:
    RetVal = SMTResultType::SMTRT_Unsat;
    break;
  case z3::check_result::unknown:
    RetVal = SMTResultType::SMTRT_Unknown;
    break;
  }

  // return to the default timeout setting
  if (Timeout > 0) {
    if (SMTSolver::GlobalTimeout > 0) {
      this->setTimeout((unsigned)SMTSolver::GlobalTimeout);
    }
  }
  return RetVal;
}

// This can filter very trivial cases, such as "x + y == 2"(sat), " x > 2 && x
// <=2"(unsat);
// TODO: currently we need to get assertions from solver, and then apply a
// tactic; better to provide a API for z3 solver.
SMTSolver::SMTResultType SMTSolver::fastCheck(unsigned Timeout) {
  SMTExpr Whole = this->assertions().toAndExpr();
  // bv-fast-check() is too week. I will use a restricted form of bit-blasting
  // solver
  z3::goal G(Solver.ctx());
  G.add(Whole.Expr);
  z3::tactic fastCheckTactic =
      z3::try_for(z3::tactic(Solver.ctx(), "bv-fast-check"), Timeout);
  Z3_lbool Res = fastCheckTactic(G)[0].as_expr().bool_value();

  SMTResultType RetVal = SMTResultType::SMTRT_Unknown;

  switch (Res) {
  case Z3_L_TRUE:
    RetVal = SMTResultType::SMTRT_Sat;
    break;
  case Z3_L_FALSE:
    RetVal = SMTResultType::SMTRT_Unsat;
    break;
  default:
    RetVal = SMTResultType::SMTRT_Unknown;
    break;
  }
  return RetVal;
}

bool SMTSolver::checkN2NQueryWithUnderAppro(
    SMTExprVec &FVec, std::vector<SMTSolver::SMTResultType> &ResVec) {
  SMTExpr CommonVC = this->assertions().toAndExpr();
  SMTExpr UnderApproExpr = CommonVC && FVec.toAndExpr();
  SMTSolver TmpSol = CommonVC.Factory->createSMTSolver();
  TmpSol.add(UnderApproExpr);
  if (TmpSol.check() == SMTSolver::SMTRT_Sat) {
    for (unsigned I = 0; I < ResVec.size(); I++) {
      ResVec[I] = SMTSolver::SMTRT_Sat;
    }
    return true;
  } else {
    // TODO: should we return false, or solve the SMTExpr in FVec one-by-one, or
    // apply "unsat-core based refinement?"
    return false;
  }
}

bool SMTSolver::overApproCheckMisc(
    SMTExpr &G, SMTExprVec &FVec,
    std::vector<SMTSolver::SMTResultType> &Labels) {
  unsigned K = FVec.size();
  SMTExpr Total = G.getSMTFactory().createBoolVal(false);
  for (unsigned I = 0; I < K; I++) {
    // Merge formulas that are not determined yet
    if (Labels[I] == SMTSolver::SMTRT_Unknown) {
      Total = Total || FVec[I];
    }
  }
  if (Total.isFalse()) {
    // This indicates that no labels are Unknown, which means we can finish the
    // algorithm
    return true;
  }
  Total = Total && G;

  SMTSolver Sol = G.getSMTFactory().createSMTSolverWithTactic(
      "qfbv"); // or pp_qfbv, pp_qfbv_light
  Sol.add(Total);

  auto Res = Sol.check();
  if (Res == SMTSolver::SMTRT_Unsat) {
    // current over-appro successes!!
    for (unsigned I = 0; I < K; I++) {
      if (Labels[I] == SMTSolver::SMTRT_Unknown) {
        Labels[I] = SMTSolver::SMTRT_Unsat;
      }
    }
    // finish the algorithm.
    return true;
  } else if (Res == SMTSolver::SMTRT_Sat) {
    // Use model-based filtering/refinement
    SMTModel M = Sol.getSMTModel();
    for (unsigned I = 0; I < K; I++) {
      if (Labels[I] == SMTSolver::SMTRT_Unknown) {
        auto TmpRes = M.getBoolValue(FVec[I], true);
        if (TmpRes.second) {
          // true under this model
          if (TmpRes.first) {
            Labels[I] = SMTSolver::SMTRT_Sat;
          } // else {
            //    Labels[I] = LUndef;
            //}
        } else {
          // Exception.
          return false;
        }
      }
    }
    overApproCheckMisc(G, FVec, Labels);

  } else {
    // If any call to solver times out, the remaining labels are Unknown
    // This is not a good idea..
    return false;
  }

  return true;
}

bool SMTSolver::checkN2NQueryWithOverAppro(
    SMTExprVec &FVec, std::vector<SMTSolver::SMTResultType> &ResVec) {
  // TODO: should we initialize ResVec here or before calling this function?
  for (unsigned I = 0; I < FVec.size(); I++) {
    ResVec.push_back(SMTSolver::SMTRT_Unknown);
  }
  SMTExpr CommonVC = this->assertions().toAndExpr();

  return overApproCheckMisc(CommonVC, FVec, ResVec);

  // for (unsigned I = 0; I < FVec.size(); I++) {
  //    std::cout << ResVec[I] << '\n';
  //}
}

// instead of using the global timeout option, setting timeout explicitly for a
// SMTSolver
void SMTSolver::setTimeout(unsigned Timeout) {
  if (Timeout > 0) {
    z3::params Z3Params(Solver.ctx());
    Z3Params.set("timeout", Timeout);
    Solver.set(Z3Params);
  }
}

void SMTSolver::push() {
  try {
    Solver.push();
  } catch (z3::exception &Ex) {
    std::cerr << __FILE__ << " : " << __LINE__ << " : " << Ex << "\n";
    exit(1);
  }
}

void SMTSolver::pop(unsigned N) {
  try {
    Solver.pop(N);
  } catch (z3::exception &Ex) {
    std::cerr << __FILE__ << " : " << __LINE__ << " : " << Ex << "\n";
    exit(1);
  }
}

unsigned SMTSolver::getNumScopes() {
  return Z3_solver_get_num_scopes(Solver.ctx(), Solver);
}

void SMTSolver::add(SMTExpr E) {
  if (E.isTrue()) {
    return;
  }

  try {
    // FIXME In some cases (ar._bfd_elf_parse_eh_frame.bc),
    // simplify() will seriously affect the performance.
    Solver.add(E.Expr /*.simplify()*/);
  } catch (z3::exception &Ex) {
    std::cerr << __FILE__ << " : " << __LINE__ << " : " << Ex << "\n";
    exit(1);
  }
}

void SMTSolver::addAll(SMTExprVec EVec) {
  // TODO: more tactics
  // 1. Add one by one(the default tactic)
  // 2. Call toAnd(EVec), and add the returned formula
  // 3. Call toAnd(EVec), add add a simplified version of the returned formula
  // 4. Take the size of EVec into considerations; choose a proper method for
  // simplification
  switch (EnableLocalSimplify.getValue()) {
  case 0:
    for (unsigned I = 0; I < EVec.size(); I++) {
      add(EVec[I]);
    }
  case 1:
    add(EVec.toAndExpr());
  case 2:
    add(EVec.toAndExpr().localSimplify());
  case 3:
    add(EVec.toAndExpr().localSimplify().doConstantPropagation());
  default:
    for (unsigned I = 0; I < EVec.size(); I++) {
      add(EVec[I]);
    }
  }
  /*
   if (EnableLocalSimplify.getValue() == 1) {
   add(EVec.toAndExpr());
   //add(EVec.toAndExpr().localSimplify());
   // Turn EVec to a single Expr, and call simplify()
   //Solver.add(EVec.toAndExpr().Expr.simplify());
   }
   else {
   for (unsigned I = 0; I < EVec.size(); I++) {
   add(EVec[I]);
   }
   }
   */
}

void SMTSolver::addAll(const std::vector<SMTExpr> &EVec) {
  // TODO: add EnableLocalSimplify option for this.
  for (unsigned I = 0; I < EVec.size(); I++) {
    add(EVec[I]);
  }
}

SMTExprVec SMTSolver::assertions() {
  std::shared_ptr<z3::expr_vector> Vec =
      std::make_shared<z3::expr_vector>(Solver.assertions());
  return SMTExprVec(&getSMTFactory(), Vec);
}

void SMTSolver::reset() { Solver.reset(); }

bool SMTSolver::operator<(const SMTSolver &Solver) const {
  return ((Z3_solver)this->Solver) < ((Z3_solver)Solver.Solver);
}

SMTModel SMTSolver::getSMTModel() {
  try {
    return SMTModel(&getSMTFactory(), Solver.get_model());
  } catch (z3::exception &e) {
    std::cerr << __FILE__ << " : " << __LINE__ << " : " << e << "\n";
    exit(1);
  }
}
