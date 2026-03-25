#include "Dataflow/IFDS/Clients/IDELinearConstantAnalysis.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

namespace ifds {

// ============================================================================
// LCAResult Implementation
// ============================================================================

void LCAResult::print(llvm::raw_ostream &OS) const {
  OS << "LCAResult at line " << line_number << " (" << source_node << "):\n";
  OS << "  Variable values:\n";
  for (const auto &pair : variable_values) {
    OS << "    " << pair.first << " = " << pair.second.to_string() << "\n";
  }
}

bool LCAResult::operator==(const LCAResult &other) const {
  return line_number == other.line_number && source_node == other.source_node &&
         variable_values == other.variable_values;
}

// ============================================================================
// IDELinearConstantAnalysis Implementation
// ============================================================================

IDELinearConstantAnalysis::IDELinearConstantAnalysis() {}

IDELinearConstantAnalysis::Fact IDELinearConstantAnalysis::zero_fact() const {
  return nullptr;
}

IDELinearConstantAnalysis::FactSet
IDELinearConstantAnalysis::normal_flow(const llvm::Instruction *stmt,
                                       const llvm::Instruction *succ,
                                       const Fact &fact) {
  FactSet result;

  if (fact == zero_fact()) {
    result.insert(fact);
  }

  if (const auto *bin_op = llvm::dyn_cast<llvm::BinaryOperator>(stmt)) {
    // Binary operations: track the result
    if (fact == bin_op->getOperand(0) || fact == bin_op->getOperand(1)) {
      result.insert(fact);
      result.insert(bin_op);
    } else if (fact == bin_op) {
      // The result itself propagates
      result.insert(fact);
    } else {
      result.insert(fact);
    }
  } else if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(stmt)) {
    // Load: propagate from pointer to loaded value
    if (fact == load->getPointerOperand()) {
      result.insert(fact);
      result.insert(load);
    } else if (fact == load) {
      result.insert(fact);
    } else {
      result.insert(fact);
    }
  } else if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(stmt)) {
    // Store: propagate value to pointer location.
    // BUG (fixed): the old code re-inserted store->getPointerOperand() even
    // when fact == getPointerOperand(), which means the old value of the
    // pointer location was kept alive after the store.  In IFDS, a store
    // *kills* the previous definition of the pointer location and *generates*
    // a new one (from the stored value).  We must NOT propagate the pointer
    // fact through the store; instead we only generate it from the value fact
    // (or from zero, handled by the edge function).
    if (fact == store->getValueOperand()) {
      result.insert(fact);
      result.insert(store->getPointerOperand());
    } else if (fact == store->getPointerOperand()) {
      // The old definition of the pointer location is killed by this store.
      // Do NOT re-insert it here; the edge function will assign the new value.
      // (No-op: fall through without inserting.)
    } else {
      result.insert(fact);
    }
  } else if (const auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(stmt)) {
    // Alloca: generate the alloca instruction itself as a fact
    if (fact == zero_fact()) {
      result.insert(fact);
      result.insert(alloca);
    } else {
      result.insert(fact);
    }
  } else if (const auto *cast = llvm::dyn_cast<llvm::CastInst>(stmt)) {
    // Casts: propagate through
    if (fact == cast->getOperand(0)) {
      result.insert(fact);
      result.insert(cast);
    } else if (fact == cast) {
      result.insert(fact);
    } else {
      result.insert(fact);
    }
  } else if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(stmt)) {
    // GEP: propagate base pointer
    if (fact == gep->getPointerOperand()) {
      result.insert(fact);
      result.insert(gep);
    } else if (fact == gep) {
      result.insert(fact);
    } else {
      result.insert(fact);
    }
  } else if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(stmt)) {
    // PHI: join all incoming values
    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
      if (fact == phi->getIncomingValue(i)) {
        result.insert(fact);
        result.insert(phi);
        break;
      }
    }
    if (fact == phi) {
      result.insert(fact);
    } else if (result.empty()) {
      result.insert(fact);
    }
  } else if (const auto *select = llvm::dyn_cast<llvm::SelectInst>(stmt)) {
    // Select: propagate from both possible values
    if (fact == select->getTrueValue() || fact == select->getFalseValue()) {
      result.insert(fact);
      result.insert(select);
    } else if (fact == select) {
      result.insert(fact);
    } else {
      result.insert(fact);
    }
  } else {
    // Default: identity flow
    result.insert(fact);
  }

  return result;
}

IDELinearConstantAnalysis::FactSet
IDELinearConstantAnalysis::call_flow(const llvm::CallBase *call,
                                     const llvm::Function *callee,
                                     const Fact &fact) {
  FactSet result;

  if (fact == zero_fact()) {
    result.insert(fact);
    return result;
  }

  // Map actual parameters to formal parameters
  if (callee && !callee->isDeclaration()) {
    unsigned arg_idx = 0;
    for (const auto &arg : callee->args()) {
      if (arg_idx < call->arg_size()) {
        if (fact == call->getArgOperand(arg_idx)) {
          result.insert(&arg);
        }
      }
      ++arg_idx;
    }
  }

  return result;
}

IDELinearConstantAnalysis::FactSet IDELinearConstantAnalysis::return_flow(
    const llvm::CallBase *call, const llvm::Instruction *exit_inst,
    const llvm::Instruction *return_site, const llvm::Function *callee,
    const Fact &exit_fact, const Fact & /*call_fact*/) {
  (void)exit_inst;
  FactSet result;

  if (exit_fact == zero_fact()) {
    result.insert(exit_fact);
    return result;
  }

  // Map return value back to call site.
  // The old code guarded with exit_fact->getType()->isIntegerTy() before the
  // dyn_cast<ReturnInst>.  A ReturnInst is an Instruction, not an
  // integer-typed value, so that type check was always false and the return
  // value was never mapped back to the call site.  The correct check is simply
  // whether exit_fact is a ReturnInst with a non-void return value.
  if (const auto *ret_inst =
          llvm::dyn_cast_or_null<llvm::ReturnInst>(exit_fact)) {
    if (ret_inst->getReturnValue()) {
      result.insert(call);
    }
  }

  // Map pointer parameters back to actuals
  if (callee && !callee->isDeclaration()) {
    unsigned arg_idx = 0;
    for (const auto &arg : callee->args()) {
      if (arg_idx < call->arg_size()) {
        if (exit_fact == &arg && arg.getType()->isPointerTy()) {
          result.insert(call->getArgOperand(arg_idx));
        }
      }
      ++arg_idx;
    }
  }

  return result;
}

IDELinearConstantAnalysis::FactSet
IDELinearConstantAnalysis::call_to_return_flow(const llvm::CallBase *call,
                                               const llvm::Instruction *return_site,
                                               llvm::ArrayRef<const llvm::Function *> callees, const Fact &fact) {
  FactSet result;

  // For non-pointer facts, pass through
  // For pointer facts passed to callees, kill them (callee may modify)
  bool is_pointer_param = false;
  for (unsigned i = 0; i < call->arg_size(); ++i) {
    if (call->getArgOperand(i) == fact &&
        call->getArgOperand(i)->getType()->isPointerTy()) {
      is_pointer_param = true;
      break;
    }
  }

  if (!is_pointer_param) {
    result.insert(fact);
  }

  return result;
}

IDELinearConstantAnalysis::FactSet
IDELinearConstantAnalysis::initial_facts(const llvm::Function *main) {
  FactSet result;
  result.insert(zero_fact());

  // Function arguments start as TOP (unknown constant)
  for (const auto &arg : main->args()) {
    if (arg.getType()->isIntegerTy()) {
      result.insert(&arg);
    }
  }

  return result;
}

IDELinearConstantAnalysis::Value
IDELinearConstantAnalysis::join(const Value &v1, const Value &v2) const {
  return v1.join(v2);
}

// ============================================================================
// Edge Functions
// ============================================================================

IDELinearConstantAnalysis::EdgeFunction
IDELinearConstantAnalysis::normal_edge_function(const llvm::Instruction *stmt,
                                                const llvm::Instruction *succ,
                                                const Fact &src_fact,
                                                const Fact &tgt_fact) {
  // Identity by default
  if (src_fact == tgt_fact) {
    return create_identity();
  }

  // Constant assignment: when the zero fact generates a new fact that is a
  // ConstantInt operand of some instruction, the edge function should return
  // the constant value.  Note: llvm::ConstantInt is NOT a subclass of
  // llvm::Instruction, so dyn_cast<ConstantInt>(stmt) always fails.  Instead,
  // detect the pattern where tgt_fact is a ConstantInt value.
  if (src_fact == nullptr /* zero fact */ && tgt_fact != nullptr) {
    if (const auto *const_int = llvm::dyn_cast<llvm::ConstantInt>(tgt_fact)) {
      return create_constant(const_int->getSExtValue());
    }
  }

  // Binary operations
  if (const auto *bin_op = llvm::dyn_cast<llvm::BinaryOperator>(stmt)) {
    if (tgt_fact == bin_op) {
      unsigned opcode = bin_op->getOpcode();

      // Check if both operands are the same fact (e.g., x = y + y)
      if (bin_op->getOperand(0) == bin_op->getOperand(1)) {
        if (bin_op->getOperand(0) == src_fact) {
          // x = y + y -> value * 2
          if (opcode == llvm::Instruction::Add) {
            return create_linear(2, 0);
          }
        }
      }

      // One operand is the source fact, other is constant
      llvm::Optional<int64_t> const_val;

      if (bin_op->getOperand(0) == src_fact) {
        const_val = as_const(bin_op->getOperand(1));
      } else if (bin_op->getOperand(1) == src_fact) {
        const_val = as_const(bin_op->getOperand(0));
      }

      if (const_val.hasValue()) {
        int64_t c = const_val.getValue();
        switch (opcode) {
        case llvm::Instruction::Add:
          return create_linear(1, c);
        case llvm::Instruction::Sub:
          if (bin_op->getOperand(0) == src_fact) {
            return create_linear(1, -c);
          } else {
            // src_fact is the subtrahend: c - x (non-linear)
            return create_bottom();
          }
        case llvm::Instruction::Mul:
          return create_linear(c, 0);
        case llvm::Instruction::Shl:
          if (c >= 0 && c < 64) {
            return create_linear(1LL << c, 0);
          }
          break;
        case llvm::Instruction::AShr:
        case llvm::Instruction::LShr:
          // Right-shift by a constant c is equivalent to integer division by
          // 2^c, which is not representable as an exact linear function
          // (multiplier * x + offset) with integer coefficients.  The previous
          // code used `1LL >> c` which evaluates to 0 for c >= 1, producing a
          // constant-zero function instead of the correct transformation.
          // Return bottom (non-constant) to be sound.
          return create_bottom();
        default:
          break;
        }
      }

      // Non-linear operation
      return create_bottom();
    }
  }

  // Cast operations
  if (const auto *cast = llvm::dyn_cast<llvm::CastInst>(stmt)) {
    if (tgt_fact == cast && src_fact == cast->getOperand(0)) {
      // Most casts preserve value (int<->int casts)
      return create_identity();
    }
  }

  // PHI nodes - join operation
  if (const auto *phi = llvm::dyn_cast<llvm::PHINode>(stmt)) {
    if (tgt_fact == phi) {
      // Check if this is a simple merge of the same value
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        if (phi->getIncomingValue(i) == src_fact) {
          return create_identity();
        }
      }
    }
  }

  return create_identity();
}

IDELinearConstantAnalysis::EdgeFunction
IDELinearConstantAnalysis::call_edge_function(const llvm::CallBase * /*call*/,
                                              const llvm::Function * /*callee*/,
                                              const Fact & /*src_fact*/,
                                              const Fact & /*tgt_fact*/) {
  return create_identity();
}

IDELinearConstantAnalysis::EdgeFunction
IDELinearConstantAnalysis::return_edge_function(const llvm::CallBase * /*call*/,
                                                const llvm::Function * /*callee*/,
                                                const llvm::Instruction * /*exit_inst*/,
                                                const llvm::Instruction *return_site, const Fact & /*exit_fact*/,
                                                const Fact & /*ret_fact*/) {
  (void)return_site;
  return create_identity();
}

IDELinearConstantAnalysis::EdgeFunction
IDELinearConstantAnalysis::call_to_return_edge_function(
    const llvm::CallBase * /*call*/, const llvm::Instruction *return_site,
    llvm::ArrayRef<const llvm::Function *> /*callees*/,
    const Fact & /*src_fact*/,
    const Fact & /*tgt_fact*/) {
  (void)return_site;
  return create_identity();
}

// ============================================================================
// Result Processing
// ============================================================================

std::map<std::string, std::map<unsigned, LCAResult>>
IDELinearConstantAnalysis::get_lca_results(
    const std::unordered_map<const llvm::Instruction *,
                             std::unordered_map<Fact, Value>> &all_values)
    const {

  std::map<std::string, std::map<unsigned, LCAResult>> results;

  for (const auto &pair : all_values) {
    const llvm::Instruction *inst = pair.first;
    if (!inst)
      continue;

    const llvm::Function *func = inst->getFunction();
    if (!func)
      continue;

    std::string func_name = func->getName().str();
    unsigned line = inst->getDebugLoc() ? inst->getDebugLoc().getLine() : 0;

    LCAResult result;
    result.line_number = line;
    result.source_node = func_name;
    result.ir_trace.push_back(inst);

    for (const auto &val_pair : pair.second) {
      const llvm::Value *val = val_pair.first;
      if (!val)
        continue;

      std::string var_name;
      if (const auto *inst_val = llvm::dyn_cast<llvm::Instruction>(val)) {
        var_name = inst_val->getName().str();
      } else if (const auto *arg = llvm::dyn_cast<llvm::Argument>(val)) {
        var_name = arg->getName().str();
      } else {
        var_name = "val_" + std::to_string(reinterpret_cast<uintptr_t>(val));
      }

      result.variable_values[var_name] = val_pair.second;
    }

    results[func_name][line] = std::move(result);
  }

  return results;
}

void IDELinearConstantAnalysis::emit_text_report(
    const std::unordered_map<const llvm::Instruction *,
                             std::unordered_map<Fact, Value>> &all_values,
    llvm::raw_ostream &OS) const {

  OS << "========================================\n";
  OS << "Linear Constant Analysis Report\n";
  OS << "========================================\n\n";

  auto results = get_lca_results(all_values);

  for (const auto &func_pair : results) {
    OS << "Function: " << func_pair.first << "\n";
    OS << "----------------------------------------\n";

    for (const auto &line_pair : func_pair.second) {
      const LCAResult &result = line_pair.second;
      OS << "  Line " << result.line_number << ":\n";

      for (const auto &var_pair : result.variable_values) {
        OS << "    " << var_pair.first << " = " << var_pair.second.to_string()
           << "\n";
      }
    }

    OS << "\n";
  }
}

// ============================================================================
// Helper Methods
// ============================================================================

bool IDELinearConstantAnalysis::defines_value(const llvm::Instruction *inst) {
  return !inst->getType()->isVoidTy();
}

const llvm::Value *
IDELinearConstantAnalysis::get_defined_value(const llvm::Instruction *inst) {
  if (defines_value(inst)) {
    return inst;
  }
  return nullptr;
}

llvm::Optional<int64_t>
IDELinearConstantAnalysis::as_const(const llvm::Value *val) {
  if (const auto *const_int = llvm::dyn_cast<llvm::ConstantInt>(val)) {
    return const_int->getSExtValue();
  }
  return llvm::None;
}

llvm::Optional<int64_t> IDELinearConstantAnalysis::apply_binop(unsigned opcode,
                                                               int64_t lhs,
                                                               int64_t rhs) {
  switch (opcode) {
  case llvm::Instruction::Add:
    return lhs + rhs;
  case llvm::Instruction::Sub:
    return lhs - rhs;
  case llvm::Instruction::Mul:
    return lhs * rhs;
  case llvm::Instruction::SDiv:
  case llvm::Instruction::UDiv:
    if (rhs != 0)
      return lhs / rhs;
    break;
  case llvm::Instruction::SRem:
  case llvm::Instruction::URem:
    if (rhs != 0)
      return lhs % rhs;
    break;
  case llvm::Instruction::Shl:
    if (rhs >= 0 && rhs < 64)
      return lhs << rhs;
    break;
  case llvm::Instruction::AShr:
    if (rhs >= 0 && rhs < 64)
      return lhs >> rhs;
    break;
  case llvm::Instruction::And:
    return lhs & rhs;
  case llvm::Instruction::Or:
    return lhs | rhs;
  case llvm::Instruction::Xor:
    return lhs ^ rhs;
  default:
    break;
  }
  return llvm::None;
}

bool IDELinearConstantAnalysis::is_linear_operation(
    const llvm::Instruction *inst) {
  if (const auto *bin_op = llvm::dyn_cast<llvm::BinaryOperator>(inst)) {
    unsigned opcode = bin_op->getOpcode();
    // Check if it's a linear operation
    if (opcode == llvm::Instruction::Add || opcode == llvm::Instruction::Sub ||
        opcode == llvm::Instruction::Mul || opcode == llvm::Instruction::Shl ||
        opcode == llvm::Instruction::AShr ||
        opcode == llvm::Instruction::LShr) {

      // Check if at least one operand is constant
      if (as_const(bin_op->getOperand(0)).hasValue() ||
          as_const(bin_op->getOperand(1)).hasValue()) {
        return true;
      }
    }
  }
  return false;
}

llvm::Optional<int64_t>
IDELinearConstantAnalysis::compute_linear_transformation(
    const llvm::Instruction *inst, int64_t input_val) {

  if (const auto *bin_op = llvm::dyn_cast<llvm::BinaryOperator>(inst)) {
    unsigned opcode = bin_op->getOpcode();

    llvm::Optional<int64_t> const_op;
    bool input_is_lhs = false;

    if (as_const(bin_op->getOperand(0)).hasValue()) {
      const_op = as_const(bin_op->getOperand(0));
      input_is_lhs = false;
    } else if (as_const(bin_op->getOperand(1)).hasValue()) {
      const_op = as_const(bin_op->getOperand(1));
      input_is_lhs = true;
    }

    if (!const_op.hasValue()) {
      return llvm::None;
    }

    int64_t c = const_op.getValue();

    switch (opcode) {
    case llvm::Instruction::Add:
      return input_val + c;
    case llvm::Instruction::Sub:
      if (input_is_lhs) {
        return input_val - c;
      } else {
        return c - input_val;
      }
    case llvm::Instruction::Mul:
      return input_val * c;
    case llvm::Instruction::Shl:
      if (c >= 0 && c < 64) {
        return input_val << c;
      }
      break;
    case llvm::Instruction::AShr:
    case llvm::Instruction::LShr:
      if (c >= 0 && c < 64) {
        return input_val >> c;
      }
      break;
    default:
      break;
    }
  }

  return llvm::None;
}

// ============================================================================
// Edge Function Helpers
// ============================================================================

IDELinearConstantAnalysis::EdgeFunction
IDELinearConstantAnalysis::create_identity() const {
  return [](const Value &v) { return v; };
}

IDELinearConstantAnalysis::EdgeFunction
IDELinearConstantAnalysis::create_constant(int64_t val) const {
  return [val](const Value & /*v*/) { return Value::constant(val); };
}

IDELinearConstantAnalysis::EdgeFunction
IDELinearConstantAnalysis::create_linear(int64_t multiplier,
                                         int64_t offset) const {
  return [multiplier, offset](const Value &v) {
    if (v.is_bottom())
      return Value::bottom();
    if (v.is_top())
      return Value::top();
    if (v.is_constant()) {
      int64_t result = multiplier * v.get_constant() + offset;
      return Value::constant(result);
    }
    return Value::bottom();
  };
}

IDELinearConstantAnalysis::EdgeFunction
IDELinearConstantAnalysis::create_bottom() const {
  return [](const Value & /*v*/) { return Value::bottom(); };
}

} // namespace ifds
