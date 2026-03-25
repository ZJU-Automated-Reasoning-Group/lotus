#ifndef CHECKER_PULSE_PULSETRANSITIVEINFO_H
#define CHECKER_PULSE_PULSETRANSITIVEINFO_H

#include <map>
#include <set>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

namespace llvm {
class Function;
class Instruction;
} // namespace llvm

namespace pulse {

/**
 * TransitiveInfo: tracks interprocedural information
 * Records transitive accesses and call resolutions
 */
class TransitiveInfo {
public:
  struct AccessInfo {
    const llvm::Instruction *location;
    const llvm::Function *function;

    AccessInfo(const llvm::Instruction *loc, const llvm::Function *func)
        : location(loc), function(func) {}
  };

  enum class CallResolution {
    Resolved,       // Call was resolved to a specific function
    Unresolved,     // Call could not be resolved
    MultipleTargets // Call has multiple possible targets
  };

  struct CallInfo {
    const llvm::Instruction *call_site;
    const llvm::Function *caller;
    CallResolution resolution;
    std::vector<const llvm::Function *> targets;

    CallInfo(const llvm::Instruction *cs, const llvm::Function *c,
             CallResolution r)
        : call_site(cs), caller(c), resolution(r) {}
  };

private:
  std::vector<AccessInfo> transitive_accesses_;
  std::vector<CallInfo> call_resolutions_;

public:
  TransitiveInfo() = default;

  /**
   * Record a transitive access
   */
  void recordTransitiveAccess(const llvm::Instruction *loc,
                              const llvm::Function *func) {
    transitive_accesses_.emplace_back(loc, func);
  }

  /**
   * Record call resolution
   */
  void recordCallResolution(
      const llvm::Instruction *call_site, const llvm::Function *caller,
      CallResolution resolution,
      const std::vector<const llvm::Function *> &targets = {}) {
    CallInfo info(call_site, caller, resolution);
    info.targets = targets;
    call_resolutions_.push_back(info);
  }

  const std::vector<AccessInfo> &getTransitiveAccesses() const {
    return transitive_accesses_;
  }
  const std::vector<CallInfo> &getCallResolutions() const {
    return call_resolutions_;
  }

  TransitiveInfo clone() const {
    TransitiveInfo cloned;
    cloned.transitive_accesses_ = transitive_accesses_;
    cloned.call_resolutions_ = call_resolutions_;
    return cloned;
  }

  /**
   * Merge two transitive info objects
   */
  static TransitiveInfo merge(const TransitiveInfo &t1,
                              const TransitiveInfo &t2);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSETRANSITIVEINFO_H
