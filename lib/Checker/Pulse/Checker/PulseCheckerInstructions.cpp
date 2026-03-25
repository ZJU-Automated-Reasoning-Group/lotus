#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Checker/PulseCheckerUtils.h"
#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Domain/PulseInvalidation.h"
#include "Checker/Pulse/Domain/PulseTaint.h"
#include "Checker/Pulse/Interproc/PulseModels.h"
#include "Checker/Pulse/Interproc/PulseSpecialization.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseLogger.h"
#include "Checker/Pulse/Report/PulseReport.h"

#include <functional>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>

namespace pulse {

std::vector<ExecutionDomain> PulseChecker::executeInstruction(
    const llvm::Instruction *I, ExecutionDomain exec_state,
    const llvm::BasicBlock *pred, unsigned call_depth) {
  if (exec_state.isStopped())
    return {exec_state};
  auto *astate = exec_state.getAstate();
  if (!astate)
    return {exec_state};

  auto pruneInfeasible = [](ExecutionDomain &st) -> bool {
    if (st.isStopped())
      return true;
    AbductiveDomain *a = st.getAstate();
    if (!a)
      return true;
    const PulseFormula &f = a->getPathFormula();
    return f.isConsistent() && !f.isUnsat();
  };

  auto pruneStates =
      [&](std::vector<ExecutionDomain> states) -> std::vector<ExecutionDomain> {
    std::vector<ExecutionDomain> out;
    out.reserve(states.size());
    for (auto &st : states) {
      if (pruneInfeasible(st)) {
        out.push_back(std::move(st));
      }
    }
    return out;
  };

  //===--------------------------------------------------------------------===//
  // Select
  //
  // `select cond, t, f` is a value-level merge. For sound incorrectness, we do
  // not want to conflate the two alternatives. However, this engine explores a
  // single witness state per instruction step, so we pick a *single* feasible
  // branch:
  // - If the condition is provably true/false (using our limited formula),
  //   choose the corresponding operand.
  // - Otherwise, pick a deterministic representative (true operand).
  //
  // This may miss bugs (false negatives) but will not fabricate a witness.
  if (auto *Sel = llvm::dyn_cast<llvm::SelectInst>(I)) {
    if (Sel->getType()->isPointerTy()) {
      llvm::Optional<bool> truth = llvm::None;
      if (auto *ICmp = llvm::dyn_cast<llvm::ICmpInst>(Sel->getCondition())) {
        llvm::Value *op0 = ICmp->getOperand(0);
        llvm::Value *op1 = ICmp->getOperand(1);
        const bool op0_is_null = detail::isNullPointerConstantValue(op0);
        const bool op1_is_null = detail::isNullPointerConstantValue(op1);
        llvm::ICmpInst::Predicate icmp_pred = ICmp->getPredicate();

        // Decide select condition only when we can prove it.
        if (op0_is_null || op1_is_null) {
          llvm::Value *ptr = op0_is_null ? op1 : op0;
          auto ptr_opt = ops_.eval(*astate, ptr, I, pred);
          if (ptr_opt) {
            AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);
            const bool proven_null = astate->getPathFormula().isNull(canon_ptr);
            const bool proven_non_null =
                astate->getPathFormula().isNonNull(canon_ptr);
            if (icmp_pred == llvm::ICmpInst::ICMP_EQ) {
              if (proven_null)
                truth = true;
              else if (proven_non_null)
                truth = false;
            } else if (icmp_pred == llvm::ICmpInst::ICMP_NE) {
              if (proven_non_null)
                truth = true;
              else if (proven_null)
                truth = false;
            }
          }
        } else if (op0->getType()->isPointerTy() &&
                   op1->getType()->isPointerTy()) {
          auto v0_opt = ops_.eval(*astate, op0, I, pred);
          auto v1_opt = ops_.eval(*astate, op1, I, pred);
          if (v0_opt && v1_opt) {
            AbstractValue c0 = astate->getCanonical(v0_opt->addr);
            AbstractValue c1 = astate->getCanonical(v1_opt->addr);
            const bool proven_eq = astate->getPathFormula().areEqual(c0, c1);
            const bool proven_neq =
                astate->getPathFormula().areDisequal(c0, c1);
            if (icmp_pred == llvm::ICmpInst::ICMP_EQ) {
              if (proven_eq)
                truth = true;
              else if (proven_neq)
                truth = false;
            } else if (icmp_pred == llvm::ICmpInst::ICMP_NE) {
              if (proven_neq)
                truth = true;
              else if (proven_eq)
                truth = false;
            }
          }
        }
      }

      auto apply_choice = [&](ExecutionDomain &st, const llvm::Value *chosen) {
        auto *a = st.getAstate();
        if (!a)
          return;
        auto chosen_opt = ops_.eval(*a, chosen, I, pred);
        if (chosen_opt) {
          AbstractValue canon = a->getCanonical(chosen_opt->addr);
          Address result(canon);
          result.history = chosen_opt->history;
          result.history.addEvent(ValueHistory::EventKind::Unknown, I,
                                  I->getFunction());
          a->getPostStack().add(Sel, result);
        }
      };

      if (!truth) {
        // Unknown condition: fork for both branches (bounded by disjunct cap).
        ExecutionDomain t_state = exec_state.clone();
        ExecutionDomain f_state = exec_state.clone();
        apply_choice(t_state, Sel->getTrueValue());
        apply_choice(f_state, Sel->getFalseValue());
        return pruneStates({std::move(t_state), std::move(f_state)});
      } else {
        const llvm::Value *chosen =
            (*truth) ? Sel->getTrueValue() : Sel->getFalseValue();
        apply_choice(exec_state, chosen);
      }
    }
    return pruneStates({exec_state});
  }

  if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(I)) {
    // PHI nodes need special handling: they merge values from multiple
    // predecessors Since we've already joined states at block entry, we can use
    // any predecessor In a more precise implementation, we'd track which
    // predecessor led to which state
    if (!pred) {
      pred = exec_state.getEntryPred();
    }

    // Check if pred is actually a predecessor of the PHI node
    bool is_predecessor = false;
    for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i) {
      if (Phi->getIncomingBlock(i) == pred) {
        is_predecessor = true;
        break;
      }
    }
    if (!is_predecessor) {
      // Without a matching predecessor, do not guess an incoming edge: it can
      // fabricate a witness. Fall back to an unknown fresh value.
      AbstractValue fresh = factory_.createFresh(Phi);
      Address addr(fresh);
      addr.history.addEvent(ValueHistory::EventKind::Unknown, I,
                            I->getFunction());
      astate->getPostStack().add(Phi, addr);
      return pruneStates({exec_state});
    }

    const llvm::Value *incoming = Phi->getIncomingValueForBlock(pred);
    auto addr_opt = ops_.eval(*astate, incoming, I, pred);
    if (addr_opt) {
      // Canonicalize the PHI result
      AbstractValue canon_addr = astate->getCanonical(addr_opt->addr);
      Address canon_result(canon_addr);
      canon_result.history = addr_opt->history;
      canon_result.history.addEvent(ValueHistory::EventKind::Unknown, I,
                                    I->getFunction());
      astate->getPostStack().add(Phi, canon_result);
    }
    return pruneStates({exec_state});
  }

  if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(I))
    return pruneStates({handleLoad(LI, exec_state, pred)});
  if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(I))
    return pruneStates({handleStore(SI, exec_state, pred)});
  if (auto *CI = llvm::dyn_cast<llvm::CallInst>(I))
    return pruneStates(handleCall(CI, exec_state, pred, call_depth));
  if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(I))
    return pruneStates({handleAlloca(AI, exec_state)});
  if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(I))
    return pruneStates({handleReturn(RI, exec_state)});

  // Integer arithmetic: record constraints linking SSA values.
  if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(I)) {
    if (BO->getType()->isIntegerTy()) {
      auto lhs_opt = ops_.eval(*astate, BO->getOperand(0), BO, pred);
      auto rhs_opt = ops_.eval(*astate, BO->getOperand(1), BO, pred);
      if (lhs_opt && rhs_opt) {
        AbstractValue res_av = factory_.getOrCreate(BO);
        AbstractValue lhs_av = astate->getCanonical(lhs_opt->addr);
        AbstractValue rhs_av = astate->getCanonical(rhs_opt->addr);
        astate->getPathFormula().addIntegerConstraint(res_av);
        astate->getPathFormula().addIntegerConstraint(lhs_av);
        astate->getPathFormula().addIntegerConstraint(rhs_av);

        std::string op;
        switch (BO->getOpcode()) {
        case llvm::Instruction::Add:
          op = "+";
          break;
        case llvm::Instruction::Sub:
          op = "-";
          break;
        case llvm::Instruction::Mul:
          op = "*";
          break;
        case llvm::Instruction::SDiv:
        case llvm::Instruction::UDiv:
          op = "/";
          break;
        case llvm::Instruction::SRem:
        case llvm::Instruction::URem:
          op = "%";
          break;
        case llvm::Instruction::And:
          op = "&";
          break;
        case llvm::Instruction::Or:
          op = "|";
          break;
        case llvm::Instruction::Xor:
          op = "^";
          break;
        case llvm::Instruction::Shl:
          op = "<<";
          break;
        case llvm::Instruction::LShr:
        case llvm::Instruction::AShr:
          op = ">>";
          break;
        default:
          break;
        }
        if (!op.empty()) {
          (void)astate->getPathFormula().addArithmeticOperation(res_av, lhs_av,
                                                                rhs_av, op);
        }
      }
    }
    return pruneStates({exec_state});
  }

  // Handle comparisons for path conditions. Skip when used as branch condition:
  // we fork and apply per-branch in applyBranchCondition.
  if (llvm::isa<llvm::ICmpInst>(I) || llvm::isa<llvm::FCmpInst>(I)) {
    const llvm::Instruction *next = I->getNextNode();
    if (auto *BI = next ? llvm::dyn_cast<llvm::BranchInst>(
                              const_cast<llvm::Instruction *>(next))
                        : nullptr) {
      if (BI->isConditional() &&
          BI->getCondition() ==
              static_cast<llvm::Value *>(const_cast<llvm::Instruction *>(I)))
        return {exec_state};
    }
    return pruneStates({handleComparison(I, exec_state, pred)});
  }

  return pruneStates({exec_state});
}

std::vector<ExecutionDomain>
PulseChecker::handleLibraryCall(const llvm::CallInst *CI,
                                ExecutionDomain exec_state,
                                const llvm::BasicBlock *pred) {

  // Delegate to modular models
  auto result = models_->dispatch(CI, exec_state, pred);
  if (result.handled) {
    return result.states;
  }

  return {};
}

ExecutionDomain PulseChecker::handleLoad(const llvm::LoadInst *LI,
                                         ExecutionDomain exec_state,
                                         const llvm::BasicBlock *pred) {
  auto *astate = exec_state.getAstate();
  const llvm::Value *ptr_operand = LI->getPointerOperand();

  // Check if this is a direct load from a stack variable (alloca)
  if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(ptr_operand)) {
    // Direct load from alloca - read from stack
    AbstractValue stack_var = factory_.getOrCreate(AI);
    AbstractValue canon_var = astate->getCanonical(stack_var);

    // Check if variable is uninitialized (only for reads, not writes)
    if (astate->getPostAttrs().has(canon_var, Attribute::Uninitialized)) {
      Trace trace;
      trace.addEvent(LI, "Load from uninitialized variable");
      if (LatentIssue::isManifest(OperationResult::UninitializedRead, *astate,
                                  canon_var)) {
        reportBug(OperationResult::UninitializedRead, LI, canon_var, trace,
                  astate);
        return ExecutionDomain::abortProgram(
            std::make_unique<AbductiveDomain>(astate->clone()),
            OperationResult::UninitializedRead, std::move(trace));
      } else {
        latent_issues_.emplace_back(OperationResult::UninitializedRead,
                                    LatentIssue::issueKindFromResult(
                                        OperationResult::UninitializedRead),
                                    canon_var, LI, std::move(trace));
        return ExecutionDomain::latentAbortProgram(
            std::make_unique<AbductiveDomain>(astate->clone()),
            &latent_issues_.back());
      }
    }

    // Variable is initialized - get value from stack
    if (auto *stack_addr = astate->getPostStack().find(AI)) {
      AbstractValue loaded_canon = astate->getCanonical(stack_addr->addr);

      // Check if loaded value is invalid (use-after-free)
      if (astate->getPostAttrs().has(loaded_canon, Attribute::Invalid)) {
        Trace trace = Trace::fromValueHistory(stack_addr->history);
        trace.addEvent(LI, "Load of freed pointer");
        if (LatentIssue::isManifest(OperationResult::UseAfterFree, *astate,
                                    loaded_canon)) {
          reportBug(OperationResult::UseAfterFree, LI, loaded_canon, trace,
                    astate);
          return ExecutionDomain::abortProgram(
              std::make_unique<AbductiveDomain>(astate->clone()),
              OperationResult::UseAfterFree, std::move(trace));
        } else {
          latent_issues_.emplace_back(
              OperationResult::UseAfterFree,
              LatentIssue::issueKindFromResult(OperationResult::UseAfterFree),
              loaded_canon, LI, std::move(trace));
          return ExecutionDomain::latentAbortProgram(
              std::make_unique<AbductiveDomain>(astate->clone()),
              &latent_issues_.back());
        }
      }

      // Do NOT report NullDereference here: loading a pointer value is not a
      // dereference. The actual dereference (store/load through the pointer) is
      // checked in writeDeref/readDeref.

      astate->getPostStack().add(LI, *stack_addr);
    } else {
      // Variable not in stack yet - create fresh value (shouldn't happen for
      // initialized)
      AbstractValue fresh = factory_.createFresh(LI);
      Address addr(fresh);
      astate->getPostStack().add(LI, addr);
    }
    return exec_state;
  }

  // Check if pointer operand is in stack map (indirect stack variable)
  if (auto *stack_addr = astate->getPostStack().find(ptr_operand)) {
    // Load from stack variable - check if uninitialized
    AbstractValue canon_ptr = astate->getCanonical(stack_addr->addr);
    AbstractValue loaded_ptr = canon_ptr;

    // PRIORITY ORDER: Invalid > Null > Uninitialized
    // Check Invalid FIRST (UseAfterFree is most severe)
    if (astate->getPostAttrs().has(loaded_ptr, Attribute::Invalid)) {
      Trace trace = Trace::fromValueHistory(stack_addr->history);
      trace.addEvent(LI, "Load of invalid pointer");
      if (LatentIssue::isManifest(OperationResult::UseAfterFree, *astate,
                                  loaded_ptr)) {
        reportBug(OperationResult::UseAfterFree, LI, loaded_ptr, trace, astate);
        return ExecutionDomain::abortProgram(
            std::make_unique<AbductiveDomain>(astate->clone()),
            OperationResult::UseAfterFree, std::move(trace));
      } else {
        latent_issues_.emplace_back(
            OperationResult::UseAfterFree,
            LatentIssue::issueKindFromResult(OperationResult::UseAfterFree),
            loaded_ptr, LI, std::move(trace));
        return ExecutionDomain::latentAbortProgram(
            std::make_unique<AbductiveDomain>(astate->clone()),
            &latent_issues_.back());
      }
    }

    // Do NOT report NullDereference here: loading a pointer value is not a
    // dereference. The actual dereference (store/load through the pointer) is
    // checked in writeDeref/readDeref.

    // HEAP POINTER CHECK: If loaded value is a heap pointer (Allocated),
    // and we're loading from it, we need to read from heap
    // This handles: int *q = *pp; where pp points to heap
    if (astate->getPostAttrs().has(loaded_ptr, Attribute::Allocated) &&
        !astate->getPostAttrs().has(loaded_ptr, Attribute::Stack)) {
      Address heap_ptr_addr(loaded_ptr);
      heap_ptr_addr.history = stack_addr->history;
      auto read_result = ops_.readDeref(*astate, heap_ptr_addr, LI);
      if (read_result.first == OperationResult::Success && read_result.second) {
        // Check if the value loaded from heap is Invalid
        AbstractValue heap_loaded_canon =
            astate->getCanonical(read_result.second->addr);
        if (astate->getPostAttrs().has(heap_loaded_canon, Attribute::Invalid)) {
          Trace trace = Trace::fromValueHistory(read_result.second->history);
          trace.addEvent(LI, "Load of invalid pointer from heap");
          if (LatentIssue::isManifest(OperationResult::UseAfterFree, *astate,
                                      heap_loaded_canon)) {
            reportBug(OperationResult::UseAfterFree, LI, heap_loaded_canon,
                      trace, astate);
            return ExecutionDomain::abortProgram(
                std::make_unique<AbductiveDomain>(astate->clone()),
                OperationResult::UseAfterFree, std::move(trace));
          } else {
            latent_issues_.emplace_back(
                OperationResult::UseAfterFree,
                LatentIssue::issueKindFromResult(OperationResult::UseAfterFree),
                heap_loaded_canon, LI, std::move(trace));
            return ExecutionDomain::latentAbortProgram(
                std::make_unique<AbductiveDomain>(astate->clone()),
                &latent_issues_.back());
          }
        }
        astate->getPostStack().add(LI, *read_result.second);
        return exec_state;
      }
    }

    // Variable is initialized - use value from stack
    astate->getPostStack().add(LI, *stack_addr);
    return exec_state;
  }

  // Heap load (pointer dereference) - use readDeref
  auto ptr_opt = ops_.eval(*astate, ptr_operand, LI, pred);
  if (!ptr_opt) {
    // Eval failed - could be null/invalid pointer in GEP or other operation
    // Check if ptr_operand is a GEP that failed due to null/invalid base
    if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr_operand)) {
      auto base_opt = ops_.eval(*astate, GEP->getPointerOperand(), LI, pred);
      if (base_opt) {
        AbstractValue base_canon = astate->getCanonical(base_opt->addr);
        // PRIORITY: Check Invalid FIRST (UseAfterFree > NullDereference)
        if (astate->getPostAttrs().has(base_canon, Attribute::Invalid)) {
          Trace trace = Trace::fromValueHistory(base_opt->history);
          trace.addEvent(LI, "Array access through invalid pointer");
          reportBug(OperationResult::UseAfterFree, LI, base_canon, trace,
                    astate);
          return ExecutionDomain::abortProgram(
              std::make_unique<AbductiveDomain>(astate->clone()),
              OperationResult::UseAfterFree, std::move(trace));
        }
        // Check if base is null (only if not invalid)
        // DON'T report NullDereference if Invalid is present (UseAfterFree
        // takes priority) NPD checker: only report if source is a null constant
        if (!astate->getPostAttrs().has(base_canon, Attribute::Invalid)) {
          if (astate->getPathFormula().isNull(base_canon) ||
              (astate->getPostAttrs().has(base_canon, Attribute::Null) &&
               !astate->getPathFormula().isNonNull(base_canon))) {
            // Check if this pointer originated from a null constant
            if (PulseOperations::isNullConstantSource(*base_opt)) {
              Trace trace = Trace::fromValueHistory(base_opt->history);
              trace.addEvent(LI, "Array access through null pointer");
              reportBug(OperationResult::NullDereference, LI, base_canon, trace,
                        astate);
              return ExecutionDomain::abortProgram(
                  std::make_unique<AbductiveDomain>(astate->clone()),
                  OperationResult::NullDereference, std::move(trace));
            }
          }
        }
      }
    }
    return exec_state;
  }
  // PRIORITY: Check Invalid FIRST before dereferencing (UseAfterFree >
  // NullDereference)
  AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);
  if (astate->getPostAttrs().has(canon_ptr, Attribute::Invalid)) {
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(LI, "Dereference of invalid pointer");
    if (LatentIssue::isManifest(OperationResult::UseAfterFree, *astate,
                                canon_ptr)) {
      reportBug(OperationResult::UseAfterFree, LI, canon_ptr, trace, astate);
      return ExecutionDomain::abortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()),
          OperationResult::UseAfterFree, std::move(trace));
    } else {
      latent_issues_.emplace_back(
          OperationResult::UseAfterFree,
          LatentIssue::issueKindFromResult(OperationResult::UseAfterFree),
          canon_ptr, LI, std::move(trace));
      return ExecutionDomain::latentAbortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()),
          &latent_issues_.back());
    }
  }

  auto read_result = ops_.readDeref(*astate, *ptr_opt, LI);
  OperationResult result = read_result.first;
  llvm::Optional<Address> value_opt = read_result.second;

  if (result != OperationResult::Success) {
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(LI, "Load from invalid address");

    if (LatentIssue::isManifest(result, *astate,
                                astate->getCanonical(ptr_opt->addr))) {
      // Manifest error - report immediately
      reportBug(result, LI, ptr_opt->addr, trace, astate);
      return ExecutionDomain::abortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()), result,
          std::move(trace));
    } else {
      // Latent error - create latent issue
      latent_issues_.emplace_back(result,
                                  LatentIssue::issueKindFromResult(result),
                                  ptr_opt->addr, LI, std::move(trace));
      return ExecutionDomain::latentAbortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()),
          &latent_issues_.back());
    }
  }
  if (value_opt) {
    // Check if the loaded value itself is Invalid (UAF through pointer)
    AbstractValue loaded_canon = astate->getCanonical(value_opt->addr);
    if (astate->getPostAttrs().has(loaded_canon, Attribute::Invalid)) {
      Trace trace = Trace::fromValueHistory(value_opt->history);
      trace.addEvent(LI, "Load of invalid/freed pointer value");
      if (LatentIssue::isManifest(OperationResult::UseAfterFree, *astate,
                                  loaded_canon)) {
        reportBug(OperationResult::UseAfterFree, LI, loaded_canon, trace,
                  astate);
        return ExecutionDomain::abortProgram(
            std::make_unique<AbductiveDomain>(astate->clone()),
            OperationResult::UseAfterFree, std::move(trace));
      } else {
        latent_issues_.emplace_back(
            OperationResult::UseAfterFree,
            LatentIssue::issueKindFromResult(OperationResult::UseAfterFree),
            loaded_canon, LI, std::move(trace));
        return ExecutionDomain::latentAbortProgram(
            std::make_unique<AbductiveDomain>(astate->clone()),
            &latent_issues_.back());
      }
    }

    astate->getPostStack().add(LI, *value_opt);

    // Check for taint sink: if loaded value flows to a sink
    // (This would be checked at sink locations, not here)
  }
  return exec_state;
}

ExecutionDomain PulseChecker::handleStore(const llvm::StoreInst *SI,
                                          ExecutionDomain exec_state,
                                          const llvm::BasicBlock *pred) {
  auto *astate = exec_state.getAstate();
  auto value_opt = ops_.eval(*astate, SI->getValueOperand(), SI, pred);
  if (!value_opt)
    return exec_state;

  auto isReallocResult = [](const Address &addr) {
    for (const auto &event : addr.history.getEvents()) {
      if (event.kind != ValueHistory::EventKind::Allocation ||
          !event.location) {
        continue;
      }
      auto *call = llvm::dyn_cast<llvm::CallInst>(event.location);
      if (!call)
        continue;
      auto *callee = call->getCalledFunction();
      if (callee && callee->getName() == "realloc") {
        return true;
      }
    }
    return false;
  };

  const llvm::Value *ptr_operand = SI->getPointerOperand();

  // Sound incorrectness: report stack address escape only when provable.
  // If a stack-derived pointer is stored into heap/global memory, it escapes.
  auto maybeReportStackEscape = [&](const Address &stored_value,
                                    const Address &dest_ptr) {
    if (!SI->getValueOperand()->getType()->isPointerTy())
      return;
    AbstractValue canon_value = astate->getCanonical(stored_value.addr);
    if (!astate->getPostAttrs().has(canon_value, Attribute::Stack))
      return;
    AbstractValue canon_dest = astate->getCanonical(dest_ptr.addr);
    const bool dest_is_global =
        astate->getPostAttrs().has(canon_dest, Attribute::Global);
    const bool dest_is_heap =
        astate->getPostAttrs().has(canon_dest, Attribute::Allocated) &&
        !astate->getPostAttrs().has(canon_dest, Attribute::Stack);
    if (!dest_is_global && !dest_is_heap)
      return; // Cannot prove escape.

    Trace trace = Trace::fromValueHistory(stored_value.history);
    trace.addEvent(SI, "Storing stack-derived address into non-stack memory");
    auto diag = std::make_unique<StackVariableAddressEscape>(
        SI, canon_value, "Stack address escapes via store",
        "Do not store addresses of local variables into heap/global memory.",
        std::move(trace));
    DiagnosticManager::getInstance().report(std::move(diag));
  };

  // Check if this is a direct store to a stack variable (alloca)
  // Stack variables are in the stack map, and we can store to them directly
  // without checking for uninitialized (storing initializes them)
  if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(ptr_operand)) {
    // Direct store to alloca - update stack and clear Uninitialized
    AbstractValue stack_var = factory_.getOrCreate(AI);
    AbstractValue canon_var = astate->getCanonical(stack_var);

    // Clear Uninitialized attribute (storing initializes the variable)
    astate->getPostAttrs().remove(canon_var, Attribute::Uninitialized);

    // When storing a pointer value, preserve its attributes (Null, Invalid,
    // Allocated)
    AbstractValue canon_value = astate->getCanonical(value_opt->addr);
    if (astate->getPostAttrs().has(canon_value, Attribute::Null)) {
      // Preserve null attribute on the stored value
      astate->getPostAttrs().add(canon_value, Attribute::Null);
    }
    if (astate->getPostAttrs().has(canon_value, Attribute::Invalid)) {
      // Preserve invalid attribute on the stored value
      astate->getPostAttrs().add(canon_value, Attribute::Invalid);
    }
    if (astate->getPostAttrs().has(canon_value, Attribute::Allocated)) {
      // Preserve allocated attribute on the stored value
      astate->getPostAttrs().add(canon_value, Attribute::Allocated);
    }

    // Fix for realloc pattern false positives: when storing a new pointer value
    // (especially from realloc), if the stored value is valid (Allocated and
    // not Invalid), clear Invalid from the variable itself. This handles cases
    // like:
    //   int *new_p = realloc(p, size);
    //   p = new_p;  // p now points to valid allocation, not the old
    //   invalidated one
    if (astate->getPostAttrs().has(canon_value, Attribute::Allocated) &&
        !astate->getPostAttrs().has(canon_value, Attribute::Invalid)) {
      // The stored value is a valid allocation - clear Invalid from the
      // variable This ensures that after p = new_p, p is not considered invalid
      astate->getPostAttrs().remove(canon_var, Attribute::Invalid);
    }

    if (astate->getPostAttrs().has(canon_value, Attribute::Allocated) &&
        !astate->getPostAttrs().has(canon_value, Attribute::Invalid) &&
        isReallocResult(*value_opt)) {
      if (auto *old_addr = astate->getPostStack().find(AI)) {
        ops_.invalidate(*astate, *old_addr, SI, InvalidationKind::Realloc);
      }
      astate->getPostAttrs().remove(canon_var, Attribute::Invalid);
    }

    // Update stack with new value
    // Add Store event to history so isNullConstantSource can detect null
    // constants stored via CallInst
    Address stored_addr = *value_opt;
    stored_addr.history.addEvent(ValueHistory::EventKind::Store, SI,
                                 SI->getFunction());
    astate->getPostStack().add(AI, stored_addr);

    // Only record copy if it's not a pointer type (to reduce false positives)
    if (!SI->getValueOperand()->getType()->isPointerTy()) {
      analysis_non_disj_.recordCopy(SI);
    }
    return exec_state;
  }

  // Check if pointer operand is in stack map (indirect stack variable)
  if (astate->getPostStack().find(ptr_operand)) {
    // Store to stack variable - clear Uninitialized and update
    auto ptr_opt = ops_.eval(*astate, ptr_operand, SI, pred);
    if (ptr_opt) {
      AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);
      astate->getPostAttrs().remove(canon_ptr, Attribute::Uninitialized);

      // Fix for realloc pattern: when storing a realloc result back to the
      // original variable, invalidate the old value (since realloc succeeded
      // and old memory is now invalid). This handles: int *new_p = realloc(p,
      // size); p = new_p;
      AbstractValue canon_value = astate->getCanonical(value_opt->addr);
      if (astate->getPostAttrs().has(canon_value, Attribute::Allocated) &&
          !astate->getPostAttrs().has(canon_value, Attribute::Invalid) &&
          isReallocResult(*value_opt)) {
        // This is a valid allocation (likely from realloc). Since we're storing
        // it back to the original variable, the old value is now invalid
        // (realloc succeeded). Invalidate the old value, then update the stack
        // map with the new value.
        ops_.invalidate(*astate, *ptr_opt, SI, InvalidationKind::Realloc);
      }

      // Update stack map with new value
      astate->getPostStack().add(ptr_operand, *value_opt);
    }
    // Only record copy if it's not a pointer type
    if (!SI->getValueOperand()->getType()->isPointerTy()) {
      analysis_non_disj_.recordCopy(SI);
    }
    return exec_state;
  }

  // Heap store (pointer dereference) - use writeDeref
  auto ptr_opt = ops_.eval(*astate, ptr_operand, SI, pred);
  if (!ptr_opt)
    return exec_state;

  // Fix for false positives: when storing to a GEP of a stack array,
  // also clear Uninitialized from the GEP address and the base alloca.
  // This prevents false positives when the array is initialized
  // element-by-element.
  if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr_operand)) {
    const llvm::Value *base = GEP->getPointerOperand();
    // Walk through any bitcasts to find the underlying alloca
    while (base) {
      if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(base)) {
        // Found the base alloca - clear Uninitialized attribute
        AbstractValue base_av = factory_.getOrCreate(AI);
        AbstractValue base_canon = astate->getCanonical(base_av);
        astate->getPostAttrs().remove(base_canon, Attribute::Uninitialized);
        break;
      }
      if (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(base)) {
        base = BC->getOperand(0);
      } else {
        break;
      }
    }
    // Also clear Uninitialized from the GEP address itself
    // (the GEP result may have inherited Uninitialized from the base)
    AbstractValue gep_canon = astate->getCanonical(ptr_opt->addr);
    astate->getPostAttrs().remove(gep_canon, Attribute::Uninitialized);
  }

  maybeReportStackEscape(*value_opt, *ptr_opt);

  auto res = ops_.writeDeref(*astate, *ptr_opt, *value_opt, SI);
  if (res != OperationResult::Success) {
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(SI, "Store to invalid address");

    if (LatentIssue::isManifest(res, *astate,
                                astate->getCanonical(ptr_opt->addr))) {
      // Manifest error - report immediately
      reportBug(res, SI, ptr_opt->addr, trace, astate);
      return ExecutionDomain::abortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()), res,
          std::move(trace));
    } else {
      // Latent error - create latent issue
      latent_issues_.emplace_back(res, LatentIssue::issueKindFromResult(res),
                                  ptr_opt->addr, SI, std::move(trace));
      return ExecutionDomain::latentAbortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()),
          &latent_issues_.back());
    }
  }
  // Only record copy if it's not a pointer type (to reduce false positives)
  if (!SI->getValueOperand()->getType()->isPointerTy()) {
    analysis_non_disj_.recordCopy(SI);
  }
  return exec_state;
}

std::vector<ExecutionDomain>
PulseChecker::handleCall(const llvm::CallInst *CI, ExecutionDomain exec_state,
                         const llvm::BasicBlock *pred, unsigned call_depth) {
  auto *astate = exec_state.getAstate();
  if (!astate) {
    return {exec_state};
  }

  // Handle LLVM lifetime intrinsics: model end as definite invalidation.
  if (auto *II = llvm::dyn_cast<llvm::IntrinsicInst>(
          const_cast<llvm::CallInst *>(CI))) {
    const auto iid = II->getIntrinsicID();
    if (iid == llvm::Intrinsic::lifetime_start ||
        iid == llvm::Intrinsic::lifetime_end) {
      if (CI->arg_size() >= 2) {
        auto ptr_opt = ops_.eval(*astate, CI->getArgOperand(1), CI, pred);
        if (ptr_opt) {
          AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);
          if (iid == llvm::Intrinsic::lifetime_start) {
            // Treat as beginning of lifetime: clear invalidation and mark
            // uninitialized.
            astate->getPostAttrs().remove(canon_ptr, Attribute::Invalid);
            astate->getPostAttrs().add(canon_ptr, Attribute::Uninitialized);
          } else {
            // End of lifetime: uses after this are invalid (gone out of scope).
            ops_.invalidate(*astate, *ptr_opt, CI,
                            InvalidationKind::GoneOutOfScope);
          }
        }
      }
      return {exec_state};
    }

    if (iid == llvm::Intrinsic::memcpy || iid == llvm::Intrinsic::memmove ||
        iid == llvm::Intrinsic::memset) {
      auto *a = exec_state.getAstate();
      if (!a)
        return {exec_state};
      auto check_len = [&](const Address &addr, const llvm::Value *len_val,
                           const char *detail) {
        auto *CI_len = llvm::dyn_cast<llvm::ConstantInt>(len_val);
        if (!CI_len || CI_len->isNegative() || CI_len->getBitWidth() > 64)
          return;
        uint64_t len = CI_len->getZExtValue();
        AbstractValue canon = a->getCanonical(addr.addr);
        auto size_opt = a->getAllocationSize(canon);
        if (size_opt && len > *size_opt) {
          Trace trace = Trace::fromValueHistory(addr.history);
          trace.addEvent(CI, detail);
          reportBug(OperationResult::OutOfBounds, CI, canon, trace, a);
          a->getPostAttrs().add(canon, Attribute::OutOfBounds);
        }
      };

      auto dest_opt = ops_.eval(*a, CI->getArgOperand(0), CI, pred);
      if (!dest_opt)
        return {exec_state};
      if (iid == llvm::Intrinsic::memset) {
        check_len(*dest_opt, CI->getArgOperand(2),
                  "memset writes beyond destination buffer");
        auto val_opt = ops_.eval(*a, CI->getArgOperand(1), CI, pred);
        if (val_opt) {
          ops_.writeDeref(*a, *dest_opt, *val_opt, CI);
        }
        a->getPostStack().add(CI, *dest_opt);
        // Fix for false positives: memset to a stack array initializes it.
        auto *dest_val = CI->getArgOperand(0);
        while (dest_val) {
          if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(dest_val)) {
            AbstractValue base_av = factory_.getOrCreate(AI);
            AbstractValue base_canon = a->getCanonical(base_av);
            a->getPostAttrs().remove(base_canon, Attribute::Uninitialized);
            break;
          }
          if (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(dest_val)) {
            dest_val = BC->getOperand(0);
          } else if (auto *GEP =
                         llvm::dyn_cast<llvm::GetElementPtrInst>(dest_val)) {
            dest_val = GEP->getPointerOperand();
          } else {
            break;
          }
        }
        return {exec_state};
      }

      auto src_opt = ops_.eval(*a, CI->getArgOperand(1), CI, pred);
      if (src_opt) {
        check_len(*dest_opt, CI->getArgOperand(2),
                  "memcpy/memmove writes beyond destination buffer");
        check_len(*src_opt, CI->getArgOperand(2),
                  "memcpy/memmove reads beyond source buffer");
        auto src_read = ops_.readDeref(*a, *src_opt, CI);
        if (src_read.first == OperationResult::Success) {
          AbstractValue dummy = factory_.createFresh(CI);
          ops_.writeDeref(*a, *dest_opt, Address(dummy), CI);
          a->getPostStack().add(CI, *dest_opt);
        }
      }
      // Fix for false positives: memcpy to a stack array initializes it,
      // so clear the Uninitialized attribute from the base alloca.
      auto *dest_val = CI->getArgOperand(0);
      while (dest_val) {
        if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(dest_val)) {
          AbstractValue base_av = factory_.getOrCreate(AI);
          AbstractValue base_canon = a->getCanonical(base_av);
          a->getPostAttrs().remove(base_canon, Attribute::Uninitialized);
          break;
        }
        if (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(dest_val)) {
          dest_val = BC->getOperand(0);
        } else if (auto *GEP =
                       llvm::dyn_cast<llvm::GetElementPtrInst>(dest_val)) {
          dest_val = GEP->getPointerOperand();
        } else {
          break;
        }
      }
      return {exec_state};
    }
  }

  llvm::Function *F = CI->getCalledFunction();
  if (!F)
    return {exec_state};

  // Check for taint sources/sinks/sanitizers in function calls
  std::string func_name = F->getName().str();
  if (models_->isTaintSource(func_name)) {
    // Mark return value as tainted (use procedure-name overload to avoid
    // dereferencing CI, which can cause EXC_BAD_ACCESS on some bitcode).
    AbstractValue ret_val = factory_.createFresh(CI);
    TaintKind kind = TaintKind::UserInput();
    if (func_name == "recv" || func_name == "recvfrom" ||
        func_name == "recvmsg") {
      kind = TaintKind::Network();
    } else if (func_name == "getenv") {
      kind = TaintKind::Environment();
    } else if (func_name == "getcwd") {
      kind = TaintKind::FileSystem();
    }
    TaintOperations::taint(*astate, ret_val, kind, func_name);
    astate->getPostStack().add(CI, Address(ret_val));
    return {exec_state};
  }

  if (models_->isTaintSink(func_name)) {
    // Check if arguments are tainted
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      auto arg_opt = ops_.eval(*astate, CI->getArgOperand(i), CI, pred);
      if (arg_opt) {
        AbstractValue canon_arg = astate->getCanonical(arg_opt->addr);
        TaintOperations::checkSink(*astate, canon_arg, func_name, CI);
      }
    }
  }

  if (models_->isTaintSanitizer(func_name)) {
    // Sanitize arguments (remove taint)
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      auto arg_opt = ops_.eval(*astate, CI->getArgOperand(i), CI, pred);
      if (arg_opt) {
        AbstractValue canon_arg = astate->getCanonical(arg_opt->addr);
        TaintKind sanitizer_kind = TaintKind::Unknown(); // Generic sanitizer
        TaintOperations::sanitize(*astate, canon_arg, sanitizer_kind, CI);
      }
    }
  }

  // Try library models first
  auto lib_result = handleLibraryCall(CI, exec_state, pred);
  if (!lib_result.empty()) {
    return lib_result; // Library model handled it
  }

  // Fall back to old handling for compatibility
  if (F->getName() == "malloc" || F->getName() == "calloc" ||
      F->getName() == "realloc") {
    AbstractValue av = factory_.createFresh(CI);
    ops_.allocate(*astate, av, CI);
    if (F->getName() == "malloc" && CI->arg_size() >= 1) {
      if (auto *CI0 = llvm::dyn_cast<llvm::ConstantInt>(CI->getArgOperand(0))) {
        if (!CI0->isNegative() && CI0->getBitWidth() <= 64) {
          astate->setAllocationSize(av, CI0->getZExtValue());
        }
      }
    } else if (F->getName() == "calloc" && CI->arg_size() >= 2) {
      auto *n = llvm::dyn_cast<llvm::ConstantInt>(CI->getArgOperand(0));
      auto *s = llvm::dyn_cast<llvm::ConstantInt>(CI->getArgOperand(1));
      if (n && s && !n->isNegative() && !s->isNegative() &&
          n->getBitWidth() <= 64 && s->getBitWidth() <= 64) {
        __int128 prod = static_cast<__int128>(n->getZExtValue()) *
                        static_cast<__int128>(s->getZExtValue());
        if (prod >= 0 && prod <= std::numeric_limits<uint64_t>::max()) {
          astate->setAllocationSize(av, static_cast<uint64_t>(prod));
        }
      }
    } else if (F->getName() == "realloc" && CI->arg_size() >= 2) {
      if (auto *CI1 = llvm::dyn_cast<llvm::ConstantInt>(CI->getArgOperand(1))) {
        if (!CI1->isNegative() && CI1->getBitWidth() <= 64) {
          astate->setAllocationSize(av, CI1->getZExtValue());
        }
      }
    }
    astate->getPostStack().add(CI, Address(av));
    return {exec_state};
  }
  if (F->getName() == "free") {
    if (CI->arg_size() > 0) {
      auto ptr_opt = ops_.eval(*astate, CI->getArgOperand(0), CI, pred);
      if (ptr_opt) {
        ops_.invalidate(*astate, *ptr_opt, CI, InvalidationKind::CFree);

        // Also invalidate aliases
        AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);
        for (auto &stack_kv : astate->getPostStack().getMap()) {
          AbstractValue stack_canon =
              astate->getCanonical(stack_kv.second.addr);
          if (stack_canon == canon_ptr) {
            astate->getPostAttrs().add(stack_canon, Attribute::Invalid);
            astate->getPostAttrs().remove(stack_canon, Attribute::Allocated);
          }
        }
      }
    }
    return {exec_state};
  }

  // Calls within the current SCC are treated as unknown to avoid
  // order-dependent interprocedural behavior in recursive cycles.
  if (current_scc_.count(F) > 0) {
    astate->addSkippedCall(F->getName().str());
    astate->declareUnknownValues();
    if (CI->getType()->isPointerTy()) {
      AbstractValue ret_val = factory_.createFresh(CI);
      Address ret_addr(ret_val);
      ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CI,
                                CI->getFunction());
      astate->getPostStack().add(CI, ret_addr);
    }
    return {exec_state};
  }

  if (F->isDeclaration()) {
    // External function with no model - record as skipped
    astate->addSkippedCall(F->getName().str());
    astate->declareUnknownValues();
    if (CI->getType()->isPointerTy()) {
      AbstractValue ret_val = factory_.createFresh(CI);
      Address ret_addr(ret_val);
      ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CI,
                                CI->getFunction());
      astate->getPostStack().add(CI, ret_addr);
    }
    return {exec_state};
  }

  // Try to use summary if available
  if (summary_manager_.hasSummary(F)) {
    PulseLogger::incrementCounter("summaries.applied");
    auto summary_results = applySummaryImproved(F, exec_state, CI, pred);
    if (!summary_results.empty()) {
      return summary_results;
    }
  }

  // No summary could be applied. Conservatively treat the call as unknown.
  astate->addSkippedCall(F->getName().str());
  astate->declareUnknownValues();
  if (CI->getType()->isPointerTy()) {
    AbstractValue ret_val = factory_.createFresh(CI);
    Address ret_addr(ret_val);
    ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CI,
                              CI->getFunction());
    astate->getPostStack().add(CI, ret_addr);
  }
  return {exec_state};
}

ExecutionDomain PulseChecker::handleAlloca(const llvm::AllocaInst *AI,
                                           ExecutionDomain exec_state) {
  auto *astate = exec_state.getAstate();
  AbstractValue av = factory_.getOrCreate(AI);
  ops_.allocate(*astate, av, AI);
  if (AI->getModule()) {
    const llvm::DataLayout &DL = AI->getModule()->getDataLayout();
    uint64_t elem_size = DL.getTypeAllocSize(AI->getAllocatedType());
    uint64_t total_size = elem_size;
    if (AI->isArrayAllocation()) {
      if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(AI->getArraySize())) {
        uint64_t count = CI->getZExtValue();
        total_size = elem_size * count;
      } else {
        total_size = 0;
      }
    }
    if (total_size > 0) {
      astate->setAllocationSize(av, total_size);
    }
  }
  // Stack allocation: mark as stack-derived so we can prove invalid escapes.
  astate->getPostAttrs().add(av, Attribute::Stack);
  astate->getPostAttrs().add(av, Attribute::Uninitialized);
  astate->getPostStack().add(AI, Address(av));
  return exec_state;
}

ExecutionDomain PulseChecker::handleReturn(const llvm::ReturnInst *RI,
                                           ExecutionDomain exec_state) {
  auto *astate = exec_state.getAstate();
  llvm::Optional<AbstractValue> returned_value = llvm::None;
  if (RI && astate && RI->getNumOperands() > 0) {
    const llvm::Value *ret_v = RI->getReturnValue();
    if (ret_v && ret_v->getType()->isPointerTy()) {
      auto ret_opt = ops_.eval(*astate, ret_v, RI, nullptr);
      if (ret_opt) {
        AbstractValue canon_ret = astate->getCanonical(ret_opt->addr);
        returned_value = canon_ret;
        if (astate->getPostAttrs().has(canon_ret, Attribute::Stack)) {
          Trace trace = Trace::fromValueHistory(ret_opt->history);
          trace.addEvent(RI, "Returning address derived from stack allocation");
          auto diag = std::make_unique<StackVariableAddressEscape>(
              RI, canon_ret, "Stack address escapes via return",
              "Do not return addresses of local variables; allocate on heap or "
              "return by value.",
              std::move(trace));
          DiagnosticManager::getInstance().report(std::move(diag));
        }
      }
    }
  }
  // Convert to ExitProgram variant
  if (exec_state.isContinueProgram() && exec_state.getAstate()) {
    return ExecutionDomain::exitProgram(
        std::make_unique<AbductiveDomain>(exec_state.getAstate()->clone()),
        returned_value);
  }
  return exec_state;
}

ExecutionDomain
PulseChecker::handleComparison(const llvm::Instruction *I,
                               ExecutionDomain exec_state,
                               const llvm::BasicBlock *pred_bb) {
  auto *astate = exec_state.getAstate();
  if (!astate)
    return exec_state;

  if (auto *ICmp = llvm::dyn_cast<llvm::ICmpInst>(I)) {
    llvm::ICmpInst::Predicate cmp_pred = ICmp->getPredicate();
    llvm::Value *op0 = ICmp->getOperand(0);
    llvm::Value *op1 = ICmp->getOperand(1);

    // Handle null pointer comparisons
    bool op0_is_null = detail::isNullPointerConstantValue(op0);
    bool op1_is_null = detail::isNullPointerConstantValue(op1);

    if (op0_is_null || op1_is_null) {
      llvm::Value *ptr = op0_is_null ? op1 : op0;
      auto ptr_opt = ops_.eval(*astate, ptr, I, pred_bb);
      if (ptr_opt) {
        AbstractValue ptr_av = astate->getCanonical(ptr_opt->addr);

        // Keep the ordering consistent with the rest of the engine:
        // Invalid (UAF) takes priority over null facts.
        bool is_invalid =
            astate->getPostAttrs().has(ptr_av, Attribute::Invalid);
        if (!is_invalid && cmp_pred == llvm::ICmpInst::ICMP_EQ) {
          // ptr == null: mark as null in path formula only (for path-sensitive
          // analysis) Don't set Null attribute here - only null constants set
          // the Null attribute This ensures NPD checker only tracks null
          // constants as sources
          astate->getPathFormula().addNull(ptr_av);
        } else if (!is_invalid && cmp_pred == llvm::ICmpInst::ICMP_NE) {
          // ptr != null: mark as non-null in formula
          astate->getPathFormula().addNonNull(ptr_av);
          // Don't remove Null attribute here - it should only be set by null
          // constants
        }
      }
    }

    // Also handle comparisons where we check if a pointer is null indirectly
    // e.g., if (ptr) or if (!ptr)
    if (op0->getType()->isPointerTy() && op1->getType()->isPointerTy()) {
      auto ptr0_opt = ops_.eval(*astate, op0, I, pred_bb);
      auto ptr1_opt = ops_.eval(*astate, op1, I, pred_bb);

      if (ptr0_opt && ptr1_opt) {
        AbstractValue av0 = astate->getCanonical(ptr0_opt->addr);
        AbstractValue av1 = astate->getCanonical(ptr1_opt->addr);

        // If comparing with zero constant (null check)
        if (op0_is_null || op1_is_null) {
          AbstractValue ptr_av = op0_is_null ? av1 : av0;
          bool is_invalid =
              astate->getPostAttrs().has(ptr_av, Attribute::Invalid);
          if (!is_invalid && cmp_pred == llvm::ICmpInst::ICMP_EQ) {
            // ptr == null: mark as null in path formula only
            // Don't set Null attribute here - only null constants set the Null
            // attribute
            astate->getPathFormula().addNull(ptr_av);
          } else if (!is_invalid && cmp_pred == llvm::ICmpInst::ICMP_NE) {
            // ptr != null: mark as non-null in formula
            astate->getPathFormula().addNonNull(ptr_av);
            // Don't remove Null attribute here - it should only be set by null
            // constants
          }
        }
      }
    }

    // Handle equality comparisons between pointers
    if (cmp_pred == llvm::ICmpInst::ICMP_EQ && op0->getType()->isPointerTy() &&
        op1->getType()->isPointerTy()) {
      auto av0_opt = ops_.eval(*astate, op0, I, pred_bb);
      auto av1_opt = ops_.eval(*astate, op1, I, pred_bb);
      if (av0_opt && av1_opt) {
        astate->addEquality(av0_opt->addr, av1_opt->addr);
      }
    }

    if (cmp_pred == llvm::ICmpInst::ICMP_NE && op0->getType()->isPointerTy() &&
        op1->getType()->isPointerTy()) {
      auto av0_opt = ops_.eval(*astate, op0, I, pred_bb);
      auto av1_opt = ops_.eval(*astate, op1, I, pred_bb);
      if (av0_opt && av1_opt) {
        astate->getPathFormula().addDisequality(av0_opt->addr, av1_opt->addr);
      }
    }
  }

  return exec_state;
}

llvm::Optional<ExecutionDomain> PulseChecker::applyBranchCondition(
    ExecutionDomain state, const llvm::BranchInst *BI, unsigned successor_index,
    const llvm::BasicBlock *pred_bb) {
  if (!BI->isConditional() || successor_index > 1)
    return llvm::Optional<ExecutionDomain>(std::move(state));
  ExecutionDomain forked = state.clone();
  auto *astate = forked.getAstate();
  if (!astate)
    return llvm::Optional<ExecutionDomain>(std::move(state));
  llvm::Value *cond = BI->getCondition();
  auto *ICmp = llvm::dyn_cast<llvm::ICmpInst>(cond);
  if (!ICmp)
    return state;
  llvm::Value *op0 = ICmp->getOperand(0);
  llvm::Value *op1 = ICmp->getOperand(1);
  llvm::ICmpInst::Predicate cmp_pred = ICmp->getPredicate();
  bool op0_null = detail::isNullPointerConstantValue(op0);
  bool op1_null = detail::isNullPointerConstantValue(op1);
  bool is_then = (successor_index == 0);

  if (op0_null || op1_null) {
    llvm::Value *ptr = op0_null ? op1 : op0;
    auto ptr_opt = ops_.eval(*astate, ptr, ICmp, pred_bb);
    if (!ptr_opt)
      return llvm::Optional<ExecutionDomain>(std::move(state));
    AbstractValue ptr_av = ptr_opt->addr;
    AbstractValue canon_ptr_av = astate->getCanonical(ptr_av);

    // PRIORITY: If pointer is already Invalid, skip null path entirely
    // This prevents false positives where we report NullDereference when
    // UseAfterFree is correct
    bool is_invalid =
        astate->getPostAttrs().has(canon_ptr_av, Attribute::Invalid);

    if (is_invalid) {
      // Pointer is invalid - only take the non-null path (where we'll report
      // UseAfterFree) Skip the null path to avoid false positive
      // NullDereference
      if (cmp_pred == llvm::ICmpInst::ICMP_EQ && is_then) {
        // This is the null path (ptr == null) - skip it
        return llvm::None;
      }
      if (cmp_pred == llvm::ICmpInst::ICMP_NE && !is_then) {
        // This is the null path (ptr != null, else branch) - skip it
        return llvm::None;
      }
    }

    if (cmp_pred == llvm::ICmpInst::ICMP_EQ) {
      if (is_then) {
        // If we already have evidence this pointer is allocated (post or pre),
        // then taking the [ptr == 0] branch makes the current path
        // contradictory. Do not silently drop it: record a latent issue so
        // callers like foo(0) can be reported.
        // EXCEPTION: malloc/calloc/realloc can return null - taking the null
        // branch is valid for these, so skip the latent issue.
        auto isFromAllocThatCanFail = [](const Address &addr) {
          for (const auto &event : addr.history.getEvents()) {
            if (event.kind == ValueHistory::EventKind::Allocation &&
                event.location) {
              if (auto *CI = llvm::dyn_cast<llvm::CallInst>(event.location)) {
                if (auto *F = CI->getCalledFunction()) {
                  llvm::StringRef name = F->getName();
                  if (name == "malloc" || name == "calloc" || name == "realloc")
                    return true;
                }
              }
            }
          }
          return false;
        };
        bool is_allocated =
            astate->getPostAttrs().has(canon_ptr_av, Attribute::Allocated) ||
            astate->getPreAttrs().has(canon_ptr_av, Attribute::Allocated);
        if (is_allocated && !isFromAllocThatCanFail(*ptr_opt)) {
          Trace trace = Trace::fromValueHistory(ptr_opt->history);
          trace.addEvent(ICmp, "Assumed null for allocated pointer");
          latent_issues_.emplace_back(OperationResult::NullDereference,
                                      LatentIssue::IssueKind::NullDereference,
                                      canon_ptr_av, ICmp, trace.clone());
          latent_issues_.back().addCallingContext(ICmp->getFunction(), ICmp);
          return llvm::Optional<ExecutionDomain>(
              ExecutionDomain::latentAbortProgram(
                  std::make_unique<AbductiveDomain>(astate->clone()),
                  &latent_issues_.back()));
        }

        if (!astate->getPathFormula().addNull(canon_ptr_av))
          return llvm::None;
        // Don't set Null attribute here - only null constants set the Null
        // attribute This ensures NPD checker only tracks null constants as
        // sources
      } else {
        astate->getPathFormula().addNonNull(canon_ptr_av);
        // Don't remove Null attribute here - it should only be set by null
        // constants
      }
    } else if (cmp_pred == llvm::ICmpInst::ICMP_NE) {
      if (is_then) {
        astate->getPathFormula().addNonNull(canon_ptr_av);
        // Don't remove Null attribute here - it should only be set by null
        // constants
      } else {
        if (!astate->getPathFormula().addNull(canon_ptr_av))
          return llvm::None;
        // Don't set Null attribute here - only null constants set the Null
        // attribute
      }
    } else {
      return llvm::Optional<ExecutionDomain>(std::move(state));
    }
    return llvm::Optional<ExecutionDomain>(std::move(forked));
  }

  // Integer comparisons contribute to path constraints too.
  if (op0->getType()->isIntegerTy() && op1->getType()->isIntegerTy()) {
    auto a0_opt = ops_.eval(*astate, op0, ICmp, pred_bb);
    auto a1_opt = ops_.eval(*astate, op1, ICmp, pred_bb);
    if (!a0_opt || !a1_opt)
      return llvm::Optional<ExecutionDomain>(std::move(state));
    AbstractValue av0 = astate->getCanonical(a0_opt->addr);
    AbstractValue av1 = astate->getCanonical(a1_opt->addr);

    llvm::ICmpInst::Predicate eff_pred =
        is_then ? cmp_pred : detail::invertIcmpPred(cmp_pred);
    if (!detail::applyIntegerIcmpConstraint(astate->getPathFormula(), eff_pred,
                                            av0, av1)) {
      return llvm::None;
    }
    return llvm::Optional<ExecutionDomain>(std::move(forked));
  }

  if (cmp_pred != llvm::ICmpInst::ICMP_EQ &&
      cmp_pred != llvm::ICmpInst::ICMP_NE)
    return llvm::Optional<ExecutionDomain>(std::move(state));

  if (op0->getType()->isPointerTy() && op1->getType()->isPointerTy()) {
    auto av0_opt = ops_.eval(*astate, op0, ICmp, pred_bb);
    auto av1_opt = ops_.eval(*astate, op1, ICmp, pred_bb);
    if (!av0_opt || !av1_opt)
      return llvm::Optional<ExecutionDomain>(std::move(state));
    AbstractValue av0 = av0_opt->addr;
    AbstractValue av1 = av1_opt->addr;

    bool should_be_equal =
        (cmp_pred == llvm::ICmpInst::ICMP_EQ) ? is_then : !is_then;
    if (should_be_equal) {
      if (!astate->getPathFormula().addEquality(av0, av1))
        return llvm::None;
    } else {
      if (!astate->getPathFormula().addDisequality(av0, av1))
        return llvm::None;
    }
    return llvm::Optional<ExecutionDomain>(std::move(forked));
  }

  return llvm::Optional<ExecutionDomain>(std::move(state));
}

} // namespace pulse
