#pragma once

#include "Solvers/SMT/LIBSMT/SMTExpr.h"
#include "Solvers/SMT/LIBSMT/SMTObject.h"
#include "z3++.h"

#include <string>

class SMTFactory;

class SMTModel : public SMTObject {
private:
  z3::model Model;

  SMTModel(SMTFactory *F, z3::model Z3Model);

public:
  SMTModel(SMTModel const &M);

  virtual ~SMTModel();

  SMTModel &operator=(const SMTModel &M);

  unsigned size();

  std::pair<std::string, std::string> getModelDbgInfo(int Index);

  friend class SMTSolver;

  SMTExpr Eval(const SMTExpr &E, bool ModelCompletion = false) const;

  std::pair<int, bool> getIntValue(const SMTExpr &E,
                                   bool ModelCompletion = false) const;

  std::pair<bool, bool> getBoolValue(const SMTExpr &E,
                                     bool ModelCompletion = false) const;

  // build a partial model that contains only variables in Vars
  // in current version, we cannot build a model explicitly with C/C++ APIs
  SMTModel getPartialModel(const SMTExprVec &Vars);

  friend std::ostream &operator<<(std::ostream &Out, SMTModel &M) {
    Out << Z3_model_to_string(M.Model.ctx(), M.Model) << "\n";
    return Out;
  }
};
