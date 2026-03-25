#include "IR/GVFG/ConditionRef.h"

#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::gvfg;

std::string ConditionRef::render() const {
  std::string buffer;
  raw_string_ostream os(buffer);

  switch (kind_) {
  case Kind::None:
    os << "<none>";
    break;
  case Kind::StructuralGuard:
    os << "guard(" << static_cast<int>(guard_kind_) << ":";
    if (control_block_)
      os << control_block_->getName();
    else
      os << "null";
    os << "->";
    if (successor_)
      os << successor_->getName();
    else
      os << "null";
    os << ")";
    break;
  case Kind::SemanticPathCond:
    if (!path_cond_) {
      os << "<null-pathcond>";
    } else {
      path_cond_->print(os);
    }
    break;
  }

  return os.str();
}
