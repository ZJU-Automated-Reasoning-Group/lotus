#ifndef GUESS_CANDIDATES__HH_
#define GUESS_CANDIDATES__HH_

#include "seahorn/HornifyModule.hh"

#include "seahorn/Expr/Expr.hh"
#include "seahorn/Expr/Smt/EZ3.hh"
#include <boost/tokenizer.hpp>
#include <fstream>
#include <iostream>

namespace seahorn
{
  //Simple templates
  ExprVector relToCand(Expr pred);
  //Load templates from file
  ExprVector applyTemplatesFromExperimentFile(Expr fdecl,
                                              const std::string &filepath);
  void parseLemmasFromExpFile(Expr bvar, ExprVector& lemmas,
                              const std::string &filepath);
} // namespace seahorn

#endif
