#pragma once

#include <deque>
#include <initializer_list>
#include <string>

#include <llvm/IR/Instructions.h>

namespace lotus {
namespace nullpointer {

/// Suffix-bounded call-string context.
///
/// When the configured limit is exceeded, the oldest call site is dropped.
/// Analyses using this context must therefore track explicit caller
/// provenance for return matching instead of reconstructing caller contexts by
/// simply popping a truncated callee context.
class CallStringContext {
public:
  CallStringContext() = default;

  CallStringContext(std::initializer_list<llvm::CallBase *> Calls)
      : Calls(Calls) {}

  bool operator==(const CallStringContext &Other) const {
    return Calls == Other.Calls;
  }

  bool operator!=(const CallStringContext &Other) const {
    return !(*this == Other);
  }

  bool operator<(const CallStringContext &Other) const {
    return Calls < Other.Calls;
  }

  bool empty() const { return Calls.empty(); }

  size_t size() const { return Calls.size(); }

  llvm::CallBase *back() const { return Calls.empty() ? nullptr : Calls.back(); }

  llvm::CallBase *popBack() {
    if (Calls.empty()) {
      return nullptr;
    }
    auto *Call = Calls.back();
    Calls.pop_back();
    return Call;
  }

  void append(llvm::CallBase *Call, unsigned Limit) {
    if (!Call || Limit == 0) {
      return;
    }
    if (Calls.size() >= Limit) {
      Calls.pop_front();
    }
    Calls.push_back(Call);
  }

  std::string str() const {
    std::string Result = "[";
    bool First = true;
    for (auto *Call : Calls) {
      if (!First) {
        Result += ", ";
      }
      First = false;
      if (Call && Call->hasName()) {
        Result += Call->getName().str();
      } else {
        Result += "<call>";
      }
    }
    Result += "]";
    return Result;
  }

  const std::deque<llvm::CallBase *> &elements() const { return Calls; }

private:
  std::deque<llvm::CallBase *> Calls;
};

} // namespace nullpointer
} // namespace lotus
