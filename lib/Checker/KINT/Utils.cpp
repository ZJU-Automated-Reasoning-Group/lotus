#include "Checker/KINT/Utils.h"

namespace kint {

int64_t get_id(llvm::Instruction *inst) {
  std::string str;
  llvm::raw_string_ostream rso(str);
  inst->print(rso);
  auto inst_str = llvm::StringRef(rso.str());
  auto ids = inst_str.trim().split(' ').first;
  size_t v = 0;
  for (auto c : ids) {
    if (!std::isdigit(c))
      continue;
    v = v * 10 + c - '0';
  }
  return static_cast<int64_t>(v);
}

} // namespace kint
