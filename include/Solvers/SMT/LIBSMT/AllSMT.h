#pragma once

#include "z3++.h"

class AllSMTSolver {
private:
  unsigned num_vars;
  unsigned num_clauses;

public:
  AllSMTSolver();

  virtual ~AllSMTSolver();

  int getModels(z3::expr &expr, int k);
};
