//===- Z3Expr.cpp -- Z3 expression (static definition) --------------------//
//
// Out-of-line definition for Z3Expr::nextId_ so the Saber tool links.
//
//===----------------------------------------------------------------------===//

#include "Checker/Saber/Z3Expr.h"

namespace lotus {
namespace analysis {

uint32_t Z3Expr::nextId_ = 0;

#ifdef USE_Z3
Z3Expr::Z3Context &Z3Expr::context() {
  static z3::context ctx;
  return ctx;
}
#endif

} // namespace analysis
} // namespace lotus
