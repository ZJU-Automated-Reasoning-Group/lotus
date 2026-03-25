/*
 * Taint Analysis Implementation - Flow Functions
 *
 * Author: rainoftime
 */

#include "Annotation/Taint/TaintConfigManager.h"
#include "Dataflow/IFDS/Clients/IFDSTaintAnalysis.h"
#include "Dataflow/IFDS/Utils/LLVMFlowHelpers.h"
#include "Utils/LLVM/Demangle.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <unordered_set>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Support/raw_ostream.h>

namespace ifds {

namespace {
std::string strip_signature(const std::string &demangled) {
  auto paren_pos = demangled.find('(');
  if (paren_pos == std::string::npos) {
    return demangled;
  }
  return demangled.substr(0, paren_pos);
}
} // namespace

// ============================================================================
// TaintAnalysis Implementation (Flow)
// ============================================================================

TaintAnalysis::TaintAnalysis() : TaintAnalysis(Config{}) {}

TaintAnalysis::TaintAnalysis(const Config &config) : m_config(config) {
  if (!taint_config::load_default_config()) {
    llvm::errs() << "Error: Could not load taint configuration\n";
    return;
  }

  auto &taint_cfg = TaintConfigManager::getInstance();
  auto sources = taint_cfg.get_all_source_functions();
  auto sinks = taint_cfg.get_all_sink_functions();

  m_source_functions.insert(sources.begin(), sources.end());
  m_sink_functions.insert(sinks.begin(), sinks.end());

  // Default sanitizers (can be extended via config file)
  if (m_config.use_sanitizers) {
    m_sanitizer_functions.insert("strlen");
    m_sanitizer_functions.insert("strcmp");
    m_sanitizer_functions.insert("strncmp");
    m_sanitizer_functions.insert("isdigit");
    m_sanitizer_functions.insert("isalpha");
    m_sanitizer_functions.insert("isalnum");
    m_sanitizer_functions.insert("isspace");
    m_sanitizer_functions.insert(
        "atoi"); // Partial sanitizer (validates numeric)
    m_sanitizer_functions.insert("atol");
    m_sanitizer_functions.insert("strtol");
    m_sanitizer_functions.insert("strtoul");
  }

  llvm::outs() << "Loaded " << sources.size() << " sources and " << sinks.size()
               << " sinks from configuration\n";
}

bool TaintAnalysis::taint_may_alias(const llvm::Value *v1,
                                    const llvm::Value *v2) const {
  if (may_alias_or_equal(v1, v2)) {
    return true;
  }
  if (!v1 || !v2) {
    return false;
  }
  const llvm::Value *base1 = llvm::getUnderlyingObject(v1);
  const llvm::Value *base2 = llvm::getUnderlyingObject(v2);
  return base1 && base2 && base1 == base2;
}

TaintFact TaintAnalysis::zero_fact() const { return TaintFact::zero(); }

TaintAnalysis::FactSet TaintAnalysis::normal_flow(const llvm::Instruction *stmt,
                                                  const llvm::Instruction *succ,
                                                  const TaintFact &fact) {
  FactSet result;

  // Always propagate zero fact
  if (fact.is_zero()) {
    result.insert(fact);
    return result;
  }

  // Helper to propagate existing facts
  auto propagate_fact = [&result, &fact]() { result.insert(fact); };

  // Helper to check if a value matches a tainted fact
  auto matches_tainted_value = [this, &fact](const llvm::Value *v) -> bool {
    if (fact.is_tainted_var() && fact.get_value() == v)
      return true;
    if (fact.is_tainted_global()) {
      if (auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(v)) {
        return fact.get_value() == gv;
      }
    }
    if (fact.is_tainted_implicit() && fact.get_value() == v)
      return true;
    return false;
  };

  if (auto *store = llvm::dyn_cast<llvm::StoreInst>(stmt)) {
    const llvm::Value *value = store->getValueOperand();
    const llvm::Value *ptr = store->getPointerOperand();

    if (matches_tainted_value(value)) {
      result.insert(TaintFact::tainted_memory(ptr, fact.get_source()));

      // Track direct and derived global stores without requiring alias-set
      // enumeration from the backend.
      if (m_config.track_globals) {
        const llvm::Value *base = llvm::getUnderlyingObject(ptr);
        if (auto *gv = llvm::dyn_cast_or_null<llvm::GlobalVariable>(base)) {
          result.insert(TaintFact::tainted_global(gv, fact.get_source()));
        }
      }
    }

    // Field-sensitive store handling
    if (m_config.field_sensitive) {
      if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr)) {
        if (matches_tainted_value(value) && gep->getNumIndices() >= 2) {
          if (auto *idx =
                  llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(2))) {
            result.insert(TaintFact::tainted_field(gep->getPointerOperand(),
                                                   idx->getSExtValue(),
                                                   fact.get_source()));
          }
        }
      }
    }

    propagate_fact();

  } else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(stmt)) {
    const llvm::Value *ptr = load->getPointerOperand();

    if ((fact.is_tainted_memory() &&
         taint_may_alias(fact.get_memory_location(), ptr)) ||
        (fact.is_tainted_var() && fact.get_value() == ptr)) {
      result.insert(TaintFact::tainted_var(load, fact.get_source()));
    }

    // Handle global variable loads
    if (m_config.track_globals && fact.is_tainted_global()) {
      if (auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(ptr)) {
        if (fact.get_value() == gv) {
          result.insert(TaintFact::tainted_var(load, fact.get_source()));
        }
      }
    }

    // Field-sensitive load handling
    if (m_config.field_sensitive && fact.is_tainted_field()) {
      if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr)) {
        if (gep->getNumIndices() >= 2) {
          if (auto *idx =
                  llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(2))) {
            if (fact.get_value() == gep->getPointerOperand() &&
                fact.get_field_index() == idx->getSExtValue()) {
              result.insert(TaintFact::tainted_var(load, fact.get_source()));
            }
          }
        }
      }
    }

    propagate_fact();

  } else if (auto *binop = llvm::dyn_cast<llvm::BinaryOperator>(stmt)) {
    const llvm::Value *lhs = binop->getOperand(0);
    const llvm::Value *rhs = binop->getOperand(1);

    if (fact.is_tainted_var() &&
        (fact.get_value() == lhs || fact.get_value() == rhs)) {
      result.insert(TaintFact::tainted_var(binop, fact.get_source()));
    }

    propagate_fact();

  } else if (auto *cmp = llvm::dyn_cast<llvm::CmpInst>(stmt)) {
    const llvm::Value *lhs = cmp->getOperand(0);
    const llvm::Value *rhs = cmp->getOperand(1);

    if (fact.is_tainted_var() &&
        (fact.get_value() == lhs || fact.get_value() == rhs)) {
      result.insert(TaintFact::tainted_var(cmp, fact.get_source()));
    }

    propagate_fact();

  } else if (auto *select = llvm::dyn_cast<llvm::SelectInst>(stmt)) {
    const llvm::Value *cond = select->getCondition();
    const llvm::Value *true_val = select->getTrueValue();
    const llvm::Value *false_val = select->getFalseValue();

    if (fact.is_tainted_var() &&
        (fact.get_value() == cond || fact.get_value() == true_val ||
         fact.get_value() == false_val)) {
      result.insert(TaintFact::tainted_var(select, fact.get_source()));
    }

    propagate_fact();

  } else if (auto *unary = llvm::dyn_cast<llvm::UnaryOperator>(stmt)) {
    const llvm::Value *operand = unary->getOperand(0);

    if (fact.is_tainted_var() && fact.get_value() == operand) {
      result.insert(TaintFact::tainted_var(unary, fact.get_source()));
    }

    propagate_fact();

  } else if (auto *cast = llvm::dyn_cast<llvm::CastInst>(stmt)) {
    if (fact.is_tainted_var() && fact.get_value() == cast->getOperand(0)) {
      result.insert(TaintFact::tainted_var(cast, fact.get_source()));
    }

    propagate_fact();

  } else if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(stmt)) {
    if (fact.is_tainted_var() && fact.get_value() == gep->getPointerOperand()) {
      result.insert(TaintFact::tainted_var(gep, fact.get_source()));
    }

    if (fact.is_tainted_memory() &&
        taint_may_alias(fact.get_memory_location(), gep->getPointerOperand())) {
      result.insert(TaintFact::tainted_memory(gep, fact.get_source()));
    }

    propagate_fact();

  } else if (auto *phi = llvm::dyn_cast<llvm::PHINode>(stmt)) {
    if (fact.is_tainted_var()) {
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        if (phi->getIncomingValue(i) == fact.get_value()) {
          result.insert(TaintFact::tainted_var(phi, fact.get_source()));
          break;
        }
      }
    }

    if (fact.is_tainted_memory() && phi->getType()->isPointerTy()) {
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        const llvm::Value *incoming = phi->getIncomingValue(i);
        if (incoming && incoming->getType()->isPointerTy() &&
            taint_may_alias(incoming, fact.get_memory_location())) {
          result.insert(TaintFact::tainted_memory(phi, fact.get_source()));
          break;
        }
      }
    }

    propagate_fact();

  } else if (auto *insert = llvm::dyn_cast<llvm::InsertValueInst>(stmt)) {
    const llvm::Value *agg = insert->getAggregateOperand();
    const llvm::Value *val = insert->getInsertedValueOperand();

    if (fact.is_tainted_var() &&
        (fact.get_value() == agg || fact.get_value() == val)) {
      result.insert(TaintFact::tainted_var(insert, fact.get_source()));
    }

    propagate_fact();

  } else if (auto *extract = llvm::dyn_cast<llvm::ExtractValueInst>(stmt)) {
    const llvm::Value *agg = extract->getAggregateOperand();

    if (fact.is_tainted_var() && fact.get_value() == agg) {
      result.insert(TaintFact::tainted_var(extract, fact.get_source()));
    }

    propagate_fact();

  } else if (auto *insert_elem =
                 llvm::dyn_cast<llvm::InsertElementInst>(stmt)) {
    const llvm::Value *vec = insert_elem->getOperand(0);
    const llvm::Value *val = insert_elem->getOperand(1);

    if (fact.is_tainted_var() &&
        (fact.get_value() == vec || fact.get_value() == val)) {
      result.insert(TaintFact::tainted_var(insert_elem, fact.get_source()));
    }

    propagate_fact();

  } else if (auto *extract_elem =
                 llvm::dyn_cast<llvm::ExtractElementInst>(stmt)) {
    const llvm::Value *vec = extract_elem->getVectorOperand();

    if (fact.is_tainted_var() && fact.get_value() == vec) {
      result.insert(TaintFact::tainted_var(extract_elem, fact.get_source()));
    }

    propagate_fact();

  } else if (auto *shuffle = llvm::dyn_cast<llvm::ShuffleVectorInst>(stmt)) {
    const llvm::Value *vec1 = shuffle->getOperand(0);
    const llvm::Value *vec2 = shuffle->getOperand(1);

    if (fact.is_tainted_var() &&
        (fact.get_value() == vec1 || fact.get_value() == vec2)) {
      result.insert(TaintFact::tainted_var(shuffle, fact.get_source()));
    }

    propagate_fact();

  } else if (auto *br = llvm::dyn_cast<llvm::BranchInst>(stmt)) {
    // Handle implicit flows from tainted branch conditions
    if (m_config.track_implicit_flows && br->isConditional()) {
      const llvm::Value *cond = br->getCondition();
      if (fact.is_tainted_var() && fact.get_value() == cond) {
        // Mark both branch targets as having implicit taint
        result.insert(TaintFact::tainted_implicit(cond, fact.get_source()));
      }
    }
    propagate_fact();

  } else if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(stmt)) {
    // Handle implicit flows from tainted switch conditions
    if (m_config.track_implicit_flows) {
      const llvm::Value *cond = sw->getCondition();
      if (fact.is_tainted_var() && fact.get_value() == cond) {
        result.insert(TaintFact::tainted_implicit(cond, fact.get_source()));
      }
    }
    propagate_fact();

  } else {
    propagate_fact();
  }

  return result;
}

TaintAnalysis::FactSet TaintAnalysis::call_flow(const llvm::CallBase *call,
                                                const llvm::Function *callee,
                                                const TaintFact &fact) {
  FactSet result;

  if (fact.is_zero()) {
    result.insert(fact);
    return result;
  }

  if (!callee || callee->isDeclaration()) {
    return result;
  }

  flow::map_facts_to_callee(
      call, callee, fact, result,
      [this](const llvm::Value *arg, const llvm::Argument * /*formal*/,
             const TaintFact &source) {
        if (source.is_tainted_var()) {
          const llvm::Value *fact_val = source.get_value();
          if (!fact_val)
            return false;
          return (arg == fact_val) ||
                 (fact_val->getType() && fact_val->getType()->isPointerTy() &&
                  taint_may_alias(arg, fact_val));
        }
        if (source.is_tainted_memory() && arg->getType() &&
            arg->getType()->isPointerTy()) {
          const llvm::Value *fact_mem = source.get_memory_location();
          return fact_mem && fact_mem->getType() &&
                 fact_mem->getType()->isPointerTy() &&
                 taint_may_alias(arg, fact_mem);
        }
        if (m_config.field_sensitive && source.is_tainted_field()) {
          return arg->getType() && arg->getType()->isPointerTy() &&
                 taint_may_alias(arg, source.get_value());
        }
        return false;
      },
      [](const llvm::Value * /*arg*/, const llvm::Argument *formal,
         const TaintFact &source) {
        if (source.is_tainted_var()) {
          return TaintFact::tainted_var(formal, source.get_source());
        }
        if (source.is_tainted_memory()) {
          return TaintFact::tainted_memory(formal, source.get_source());
        }
        return TaintFact::tainted_field(formal, source.get_field_index(),
                                        source.get_source());
      });

  // Propagate global variable taint across call boundaries
  if (m_config.track_globals && fact.is_tainted_global()) {
    result.insert(fact);
  }

  return result;
}

TaintAnalysis::FactSet TaintAnalysis::return_flow(const llvm::CallBase *call,
                                                  const llvm::Instruction *exit_inst,
                                                  const llvm::Instruction *return_site, const llvm::Function *callee,
                                                  const TaintFact &exit_fact,
                                                  const TaintFact &call_fact) {
  FactSet result;

  if (exit_fact.is_zero()) {
    result.insert(exit_fact);
    return result;
  }

  flow::map_facts_to_caller(
      call, callee, exit_fact, result,
      [this](const llvm::Argument *formal, const llvm::Value *actual,
             const TaintFact &source) {
        if (!(source.is_tainted_memory() && actual && actual->getType() &&
              actual->getType()->isPointerTy())) {
          return false;
        }
        return taint_may_alias(formal, source.get_memory_location());
      },
      [](const llvm::Argument * /*formal*/, const llvm::Value *actual,
         const TaintFact &source) {
        return TaintFact::tainted_memory(actual, source.get_source());
      },
      [](const llvm::Value *ret_val, const TaintFact &source) {
        return source.is_tainted_var() && ret_val == source.get_value();
      },
      [call](const llvm::Value * /*ret_val*/, const TaintFact &source) {
        return TaintFact::tainted_var(call, source.get_source());
      });

  if (!call_fact.is_zero()) {
    result.insert(call_fact);
  }

  return result;
}

TaintAnalysis::FactSet
TaintAnalysis::call_to_return_flow(const llvm::CallBase *call,
                                   const llvm::Instruction *return_site,
                                   llvm::ArrayRef<const llvm::Function *> callees, const TaintFact &fact) {
  FactSet result;

  const llvm::Function *callee = call->getCalledFunction();

  // Handle sources independently of incoming facts.
  // BUG (fixed): the old code called both is_source() and
  // handle_source_function_specs() unconditionally, which caused duplicate
  // taint facts to be inserted for functions that are both in the source set
  // AND have explicit source specs in the config.  We now use a single path:
  // if the function has explicit config specs, use those (they are more
  // precise); otherwise fall back to the generic is_source() treatment.
  if (callee) {
    // Try config-driven source specs first.
    FactSet source_facts;
    handle_source_function_specs(call, source_facts);
    if (!source_facts.empty()) {
      result.insert(source_facts.begin(), source_facts.end());
    } else if (is_source(call)) {
      // No explicit spec — use the generic treatment.
      if (!call->getType()->isVoidTy()) {
        result.insert(TaintFact::tainted_var(call, call));
        if (call->getType()->isPointerTy()) {
          result.insert(TaintFact::tainted_memory(call, call));
        }
      }
    }
  } else if (is_source(call)) {
    if (!call->getType()->isVoidTy()) {
      result.insert(TaintFact::tainted_var(call, call));
      if (call->getType()->isPointerTy()) {
        result.insert(TaintFact::tainted_memory(call, call));
      }
    }
  }

  if (!callee) {
    flow::map_facts_alongside_callsite_with_policies(
        call, fact, result,
        [this, call](const llvm::Value * /*arg*/, const TaintFact &source) {
          return kills_fact(call, source);
        },
        [](const TaintFact &source) { return source.is_zero(); },
        [](const TaintFact &source) { return source.is_tainted_global(); },
        /*PropagateGlobals=*/true,
        /*PropagateZero=*/true);
    return result;
  }

  // Handle PIPE specifications for taint propagation
  if (!fact.is_zero()) {
    handle_pipe_specifications(call, fact, result);
  }

  // Propagate facts that are not killed by the call (policy-based helper).
  flow::map_facts_alongside_callsite_with_policies(
      call, fact, result,
      [this, call](const llvm::Value * /*arg*/, const TaintFact &source) {
        return kills_fact(call, source);
      },
      [](const TaintFact &source) { return source.is_zero(); },
      [](const TaintFact &source) { return source.is_tainted_global(); },
      /*PropagateGlobals=*/true,
      /*PropagateZero=*/true);

  return result;
}

TaintAnalysis::FactSet
TaintAnalysis::initial_facts(const llvm::Function *main) {
  FactSet result;
  result.insert(zero_fact());

  // Taint command line arguments
  for (const llvm::Argument &arg : main->args()) {
    if (arg.getType()->isPointerTy()) {
      result.insert(TaintFact::tainted_var(&arg));
    }
  }

  return result;
}

bool TaintAnalysis::is_source(const llvm::Instruction *inst) const {
  auto *call = llvm::dyn_cast<llvm::CallInst>(inst);
  if (!call || !call->getCalledFunction())
    return false;

  auto raw_name = call->getCalledFunction()->getName().str();
  std::string func_name = taint_config::normalize_name(raw_name);
  if (m_source_functions.count(func_name) > 0) {
    return true;
  }

  // Fallback: demangle C++ names and match by suffix (e.g., "::source").
  std::string demangled_name = DemangleUtils::demangle(raw_name);
  std::string normalized_demangled =
      taint_config::normalize_name(strip_signature(demangled_name));
  for (const auto &source : m_source_functions) {
    if (normalized_demangled.size() >= source.size() &&
        normalized_demangled.compare(normalized_demangled.size() -
                                         source.size(),
                                     source.size(), source) == 0) {
      return true;
    }
  }

  return false;
}

bool TaintAnalysis::is_sink(const llvm::Instruction *inst) const {
  auto *call = llvm::dyn_cast<llvm::CallInst>(inst);
  if (!call || !call->getCalledFunction())
    return false;

  auto raw_name = call->getCalledFunction()->getName().str();
  std::string func_name = taint_config::normalize_name(raw_name);
  if (m_sink_functions.count(func_name) > 0) {
    return true;
  }

  // Fallback: demangle C++ names and match by suffix (e.g., "::sink").
  std::string demangled_name = DemangleUtils::demangle(raw_name);
  std::string normalized_demangled =
      taint_config::normalize_name(strip_signature(demangled_name));
  for (const auto &sink : m_sink_functions) {
    if (normalized_demangled.size() >= sink.size() &&
        normalized_demangled.compare(normalized_demangled.size() - sink.size(),
                                     sink.size(), sink) == 0) {
      return true;
    }
  }

  return false;
}

void TaintAnalysis::add_source_function(const std::string &func_name) {
  m_source_functions.insert(func_name);
}

void TaintAnalysis::add_sink_function(const std::string &func_name) {
  m_sink_functions.insert(func_name);
}

void TaintAnalysis::add_sanitizer_function(const std::string &func_name) {
  m_sanitizer_functions.insert(func_name);
}

bool TaintAnalysis::is_sanitizer(const llvm::Instruction *inst) const {
  if (!m_config.use_sanitizers)
    return false;

  auto *call = llvm::dyn_cast<llvm::CallInst>(inst);
  if (!call || !call->getCalledFunction())
    return false;

  auto raw_name = call->getCalledFunction()->getName().str();
  std::string func_name = taint_config::normalize_name(raw_name);

  return m_sanitizer_functions.count(func_name) > 0;
}

bool TaintAnalysis::kills_fact(const llvm::CallBase *call,
                               const TaintFact &fact) const {
  const llvm::Function *callee = call->getCalledFunction();
  if (!callee)
    return false;

  // Check if this is a sanitizer that kills the taint
  if (!m_config.use_sanitizers)
    return false;

  std::string func_name = taint_config::normalize_name(callee->getName().str());

  // Only kill if this is a recognized sanitizer
  if (m_sanitizer_functions.count(func_name) == 0)
    return false;

  // For strict sanitization, only kill facts that directly match operands
  if (fact.is_tainted_var()) {
    for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
      if (call->getOperand(i) == fact.get_value()) {
        return true;
      }
    }
  }

  // Memory taint can be sanitized if the pointer is passed to sanitizer
  if (fact.is_tainted_memory()) {
    for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
      const llvm::Value *arg = call->getOperand(i);
      if (arg && arg->getType()->isPointerTy() &&
          taint_may_alias(arg, fact.get_memory_location())) {
        return true;
      }
    }
  }

  return false;
}

// Helper function to handle source function specifications from config
void TaintAnalysis::handle_source_function_specs(const llvm::CallBase *call,
                                                 FactSet &result) const {
  std::string func_name =
      taint_config::normalize_name(call->getCalledFunction()->getName().str());
  const FunctionTaintConfig *func_config =
      taint_config::get_function_config(func_name);

  if (func_config && func_config->has_source_specs()) {
    for (const auto &spec : func_config->source_specs) {
      if (spec.location == TaintSpec::RET &&
          spec.access_mode == TaintSpec::VALUE) {
        result.insert(TaintFact::tainted_var(call));
      } else if (spec.location == TaintSpec::ARG &&
                 (spec.access_mode == TaintSpec::DIRECT_DEREF ||
                  spec.access_mode == TaintSpec::REACHABLE_DEREF)) {
        if (spec.arg_index >= 0 &&
            spec.arg_index < (int)(call->getNumOperands() - 1)) {
          const llvm::Value *arg = call->getOperand(spec.arg_index);
          if (arg->getType()->isPointerTy()) {
            result.insert(TaintFact::tainted_memory(arg));
          }
        }
      } else if (spec.location == TaintSpec::AFTER_ARG &&
                 (spec.access_mode == TaintSpec::DIRECT_DEREF ||
                  spec.access_mode == TaintSpec::REACHABLE_DEREF)) {
        unsigned start_arg = spec.arg_index + 1;
        for (unsigned i = start_arg; i < call->getNumOperands() - 1; ++i) {
          const llvm::Value *arg = call->getOperand(i);
          if (arg->getType()->isPointerTy()) {
            result.insert(TaintFact::tainted_memory(arg));
          }
        }
      }
    }
  }
}

// Helper function to handle PIPE specifications for taint propagation
void TaintAnalysis::handle_pipe_specifications(const llvm::CallBase *call,
                                               const TaintFact &fact,
                                               FactSet &result) const {
  std::string func_name =
      taint_config::normalize_name(call->getCalledFunction()->getName().str());
  const FunctionTaintConfig *func_config =
      taint_config::get_function_config(func_name);

  if (func_config && func_config->has_pipe_specs()) {
    for (const auto &pipe_spec : func_config->pipe_specs) {
      bool matches_from = false;

      // Check if current fact matches the 'from' spec
      if (pipe_spec.from.location == TaintSpec::ARG) {
        int from_arg_idx = pipe_spec.from.arg_index;
        if (from_arg_idx >= 0 &&
            from_arg_idx < (int)(call->getNumOperands() - 1)) {
          const llvm::Value *from_arg = call->getOperand(from_arg_idx);

          if (pipe_spec.from.access_mode == TaintSpec::VALUE) {
            if (fact.is_tainted_var() && fact.get_value() == from_arg) {
              matches_from = true;
            }
          } else {
            if (fact.is_tainted_memory() &&
                from_arg->getType()->isPointerTy()) {
              if (taint_may_alias(from_arg, fact.get_memory_location())) {
                matches_from = true;
              }
            }
          }
        }
      }

      // If from matches, propagate to 'to'
      if (matches_from) {
        if (pipe_spec.to.location == TaintSpec::RET) {
          if (pipe_spec.to.access_mode == TaintSpec::VALUE) {
            result.insert(TaintFact::tainted_var(call));
          } else {
            if (call->getType()->isPointerTy()) {
              result.insert(TaintFact::tainted_memory(call));
            }
          }
        } else if (pipe_spec.to.location == TaintSpec::ARG) {
          int to_arg_idx = pipe_spec.to.arg_index;
          if (to_arg_idx >= 0 &&
              to_arg_idx < (int)(call->getNumOperands() - 1)) {
            const llvm::Value *to_arg = call->getOperand(to_arg_idx);

            if (pipe_spec.to.access_mode == TaintSpec::VALUE) {
              result.insert(TaintFact::tainted_var(to_arg));
            } else {
              if (to_arg->getType()->isPointerTy()) {
                result.insert(TaintFact::tainted_memory(to_arg));
              }
            }
          }
        }
      }
    }
  }
}

// Helper function to check if an argument is tainted by a fact
bool TaintAnalysis::is_argument_tainted(const llvm::Value *arg,
                                        const TaintFact &fact) const {
  return (fact.is_tainted_var() && fact.get_value() == arg) ||
         (fact.is_tainted_memory() && arg->getType()->isPointerTy() &&
          (fact.get_memory_location() == arg ||
           may_alias(arg, fact.get_memory_location())));
}

// Helper function to format tainted argument description
std::string
TaintAnalysis::format_tainted_arg(unsigned arg_index, const TaintFact &fact,
                                  const llvm::CallBase *call) const {
  if (fact.is_tainted_var()) {
    return "arg" + std::to_string(arg_index);
  } else if (fact.is_tainted_memory()) {
    return (fact.get_memory_location() == call->getOperand(arg_index))
               ? "arg" + std::to_string(arg_index) + "(mem)"
               : "arg" + std::to_string(arg_index) + "(alias)";
  }
  return "";
}

// Helper function to analyze tainted arguments for a call
void TaintAnalysis::analyze_tainted_arguments(
    const llvm::CallBase *call, const TaintAnalysis::FactSet &facts,
    std::string &tainted_args) const {
  std::set<std::string> unique_tainted_args;

  for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
    const llvm::Value *arg = call->getOperand(i);

    for (const auto &fact : facts) {
      if (is_argument_tainted(arg, fact)) {
        std::string arg_desc = format_tainted_arg(i, fact, call);
        if (!arg_desc.empty()) {
          unique_tainted_args.insert(arg_desc);
        }
        break;
      }
    }
  }

  // Join unique tainted arguments
  for (const auto &arg_desc : unique_tainted_args) {
    if (!tainted_args.empty())
      tainted_args += ", ";
    tainted_args += arg_desc;
  }
}

} // namespace ifds
