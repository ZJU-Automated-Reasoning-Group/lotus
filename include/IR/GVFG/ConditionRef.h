#pragma once

#include "Alias/LotusAA/MemoryModel/Types.h"
#include "IR/GSA/GSA.h"

#include <string>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Value.h>

namespace lotus {
namespace gvfg {

using llvm::BasicBlock;
using llvm::ConstantInt;
using llvm::path_cond_t;
using llvm::Value;

class ConditionRef {
public:
  enum class Kind {
    None,
    StructuralGuard,
    SemanticPathCond,
  };

  ConditionRef() = default;

  static ConditionRef none() { return ConditionRef(); }

  static ConditionRef fromGuard(gsa::GuardKind kind, BasicBlock *control_block,
                                BasicBlock *successor, Value *condition,
                                ConstantInt *case_value = nullptr) {
    ConditionRef ref;
    ref.kind_ = Kind::StructuralGuard;
    ref.guard_kind_ = kind;
    ref.control_block_ = control_block;
    ref.successor_ = successor;
    ref.condition_ = condition;
    ref.case_value_ = case_value;
    return ref;
  }

  static ConditionRef fromPathCond(path_cond_t path_cond) {
    ConditionRef ref;
    ref.kind_ = Kind::SemanticPathCond;
    ref.path_cond_ = path_cond;
    return ref;
  }

  Kind getKind() const { return kind_; }
  bool isValid() const { return kind_ != Kind::None; }

  gsa::GuardKind getGuardKind() const { return guard_kind_; }
  BasicBlock *getControlBlock() const { return control_block_; }
  BasicBlock *getSuccessor() const { return successor_; }
  Value *getCondition() const { return condition_; }
  ConstantInt *getCaseValue() const { return case_value_; }
  path_cond_t getPathCond() const { return path_cond_; }

  std::string render() const;

  bool operator==(const ConditionRef &other) const {
    return kind_ == other.kind_ && guard_kind_ == other.guard_kind_ &&
           control_block_ == other.control_block_ &&
           successor_ == other.successor_ && condition_ == other.condition_ &&
           case_value_ == other.case_value_ && path_cond_ == other.path_cond_;
  }

  bool operator!=(const ConditionRef &other) const { return !(*this == other); }

private:
  Kind kind_{Kind::None};
  gsa::GuardKind guard_kind_{gsa::GuardKind::Opaque};
  BasicBlock *control_block_{nullptr};
  BasicBlock *successor_{nullptr};
  Value *condition_{nullptr};
  ConstantInt *case_value_{nullptr};
  path_cond_t path_cond_{nullptr};
};

} // namespace gvfg
} // namespace lotus
