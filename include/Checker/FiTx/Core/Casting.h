#pragma once
#include "llvm/Support/Casting.h"

#include <memory>

namespace fitx {
template <class X, class Y> bool shared_isa(std::shared_ptr<Y> value) {
  return llvm::isa<X>(*value);
}

template <class X, class Y>
std::shared_ptr<X> shared_dyn_cast(std::shared_ptr<Y> value) {
  return shared_isa<X>(value) ? std::static_pointer_cast<X>(value)
                              : std::shared_ptr<X>();
}
} // namespace fitx
