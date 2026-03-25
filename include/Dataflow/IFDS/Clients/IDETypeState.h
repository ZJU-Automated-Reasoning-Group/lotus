#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>

namespace ifds {

// ============================================================================
// Configurable Typestate Analysis Framework
// ============================================================================

/**
 * TypeStateValue - Generic typestate value with user-defined states
 *
 * This allows defining arbitrary typestate properties like:
 * - File: Closed -> Opened -> Closed | Error
 * - Lock: Unlocked -> Locked -> Unlocked | Error
 * - Memory: Unallocated -> Allocated -> Freed | Error
 * - Socket: Uninitialized -> Bound -> Listening -> Connected -> Closed
 */
class TypeStateValue {
public:
  enum SpecialState {
    TOP,   // Unknown/any state (join of conflicting states)
    BOTTOM // Unreachable/no information
  };

private:
  bool m_is_special;
  SpecialState m_special;
  int m_user_state; // User-defined state ID

public:
  // Constructors
  TypeStateValue() : m_is_special(true), m_special(BOTTOM), m_user_state(-1) {}
  explicit TypeStateValue(SpecialState s)
      : m_is_special(true), m_special(s), m_user_state(-1) {}
  explicit TypeStateValue(int state_id)
      : m_is_special(false), m_special(BOTTOM), m_user_state(state_id) {}

  bool is_special() const { return m_is_special; }
  bool is_top() const { return m_is_special && m_special == TOP; }
  bool is_bottom() const { return m_is_special && m_special == BOTTOM; }
  int user_state() const { return m_user_state; }

  bool operator==(const TypeStateValue &other) const {
    if (m_is_special != other.m_is_special)
      return false;
    if (m_is_special)
      return m_special == other.m_special;
    return m_user_state == other.m_user_state;
  }

  bool operator!=(const TypeStateValue &other) const {
    return !(*this == other);
  }
};

} // namespace ifds

namespace std {
template <> struct hash<ifds::TypeStateValue> {
  size_t operator()(const ifds::TypeStateValue &v) const {
    if (v.is_special()) {
      return std::hash<int>{}(v.is_top() ? -1 : -2);
    }
    return std::hash<int>{}(v.user_state());
  }
};
} // namespace std

namespace ifds {

/**
 * TypeStateProperty - Configuration for a typestate property
 *
 * Defines:
 * - Named states (e.g., "Closed", "Opened", "Error")
 * - Transitions triggered by operations (function calls, instructions)
 * - Error states (states that should be reported as violations)
 * - Initial state(s)
 */
class TypeStateProperty {
public:
  struct Transition {
    int from_state;
    int to_state;

    Transition(int from, int to) : from_state(from), to_state(to) {}
  };

  struct OperationMatcher {
    enum MatchType {
      FUNCTION_NAME,      // Match by called function name
      FUNCTION_REGEX,     // Match by regex on function name
      INSTRUCTION_OPCODE, // Match by LLVM opcode (e.g., "store", "load")
      CUSTOM_PREDICATE    // Custom predicate function
    };

    MatchType type;
    std::string pattern;
    std::function<bool(const llvm::Instruction *)> predicate;

    OperationMatcher(MatchType t, const std::string &p) : type(t), pattern(p) {}
    OperationMatcher(std::function<bool(const llvm::Instruction *)> &&pred)
        : type(CUSTOM_PREDICATE), predicate(std::move(pred)) {}
  };

private:
  std::string m_name;
  std::unordered_map<std::string, int> m_state_names; // name -> state_id
  std::vector<std::string> m_state_ids;               // state_id -> name
  std::unordered_set<int> m_error_states;
  int m_initial_state;
  int m_next_state_id;

  // Operation -> list of transitions
  std::vector<std::pair<OperationMatcher, std::vector<Transition>>>
      m_operation_transitions;

public:
  TypeStateProperty(const std::string &name)
      : m_name(name), m_initial_state(-1), m_next_state_id(0) {}

  // Define a state
  int define_state(const std::string &name, bool is_error = false) {
    auto it = m_state_names.find(name);
    if (it != m_state_names.end()) {
      return it->second;
    }
    int state_id = m_next_state_id++;
    m_state_names[name] = state_id;
    m_state_ids.push_back(name);
    if (is_error) {
      m_error_states.insert(state_id);
    }
    return state_id;
  }

  // Set initial state
  void set_initial_state(const std::string &name) {
    m_initial_state = get_state_id(name);
  }

  void set_initial_state(int state_id) { m_initial_state = state_id; }

  // Get state ID by name
  int get_state_id(const std::string &name) const {
    auto it = m_state_names.find(name);
    return (it != m_state_names.end()) ? it->second : -1;
  }

  // Get state name by ID
  std::string get_state_name(int state_id) const {
    return (state_id >= 0 && state_id < (int)m_state_ids.size())
               ? m_state_ids[state_id]
               : "<unknown>";
  }

  int get_initial_state() const { return m_initial_state; }
  bool is_error_state(int state_id) const {
    return m_error_states.count(state_id) > 0;
  }

  // Add transition: when operation matches, transition from -> to
  void add_transition_for_function(const std::string &func_name, int from_state,
                                   int to_state) {
    OperationMatcher matcher(OperationMatcher::FUNCTION_NAME, func_name);
    add_transition(matcher, from_state, to_state);
  }

  void add_transition_for_opcode(const std::string &opcode, int from_state,
                                 int to_state) {
    OperationMatcher matcher(OperationMatcher::INSTRUCTION_OPCODE, opcode);
    add_transition(matcher, from_state, to_state);
  }

  void add_transition_for_predicate(
      std::function<bool(const llvm::Instruction *)> &&pred, int from_state,
      int to_state) {
    OperationMatcher matcher(std::move(pred));
    add_transition(matcher, from_state, to_state);
  }

  // Query transitions
  std::vector<Transition> get_transitions(const llvm::Instruction *inst) const;

  const std::string &get_name() const { return m_name; }

private:
  void add_transition(const OperationMatcher &matcher, int from_state,
                      int to_state) {
    // Find existing matcher or create new
    for (auto &pair : m_operation_transitions) {
      if (matchers_equal(pair.first, matcher)) {
        pair.second.emplace_back(from_state, to_state);
        return;
      }
    }
    // New matcher
    std::vector<Transition> trans;
    trans.emplace_back(from_state, to_state);
    m_operation_transitions.emplace_back(matcher, trans);
  }

  bool matchers_equal(const OperationMatcher &m1,
                      const OperationMatcher &m2) const {
    if (m1.type != m2.type)
      return false;
    if (m1.type == OperationMatcher::CUSTOM_PREDICATE)
      return false; // Always unique
    return m1.pattern == m2.pattern;
  }

  bool matcher_matches(const OperationMatcher &matcher,
                       const llvm::Instruction *inst) const;
};

/**
 * IDETypeState - Parametric typestate analysis
 *
 * Usage example:
 *
 *   // Define a file typestate property
 *   auto file_prop = std::make_shared<TypeStateProperty>("File");
 *   int closed = file_prop->define_state("Closed");
 *   int opened = file_prop->define_state("Opened");
 *   int error = file_prop->define_state("Error", true);
 *   file_prop->set_initial_state(closed);
 *   file_prop->add_transition_for_function("fopen", closed, opened);
 *   file_prop->add_transition_for_function("fclose", opened, closed);
 *   file_prop->add_transition_for_function("fread", closed, error);
 *
 *   IDETypeState analysis(file_prop);
 *   IDESolver<IDETypeState> solver(analysis);
 *   solver.solve(module);
 */
class IDETypeState
    : public DefaultAliasAwareIDEProblem<const llvm::Value *, TypeStateValue> {
public:
  using Fact = const llvm::Value *;
  using Value = TypeStateValue;

  // Constructor with typestate property
  explicit IDETypeState(std::shared_ptr<TypeStateProperty> property);

  // IFDS interface
  Fact zero_fact() const override { return nullptr; }
  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ, const Fact &fact) override;
  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const Fact &fact) override;
  FactSet return_flow(const llvm::CallBase *call,
                      const llvm::Instruction *exit_inst,
                      const llvm::Instruction *return_site,
                      const llvm::Function *callee, const Fact &exit_fact,
                      const Fact &call_fact) override;
  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> callees,
                              const Fact &fact) override;
  FactSet initial_facts(const llvm::Function *main) override;
  IDEInitialSeeds initial_ide_seeds(const llvm::Module &module) override {
    return this->lift_ifds_initial_seeds(module, bottom_value());
  }

  // Value domain
  Value top_value() const override { return Value(TypeStateValue::TOP); }
  Value bottom_value() const override { return Value(TypeStateValue::BOTTOM); }
  Value join(const Value &v1, const Value &v2) const override;

  // Edge functions
  EdgeFunction normal_edge_function(const llvm::Instruction *stmt,
                                    const llvm::Instruction *succ,
                                    const Fact &src_fact,
                                    const Fact &tgt_fact) override;
  EdgeFunction call_edge_function(const llvm::CallBase *call,
                                  const llvm::Function *callee,
                                  const Fact &src_fact,
                                  const Fact &tgt_fact) override;
  EdgeFunction return_edge_function(const llvm::CallBase *call,
                                    const llvm::Function *callee,
                                    const llvm::Instruction *exit_inst,
                                    const llvm::Instruction *return_site,
                                    const Fact &exit_fact,
                                    const Fact &ret_fact) override;
  EdgeFunction call_to_return_edge_function(
      const llvm::CallBase *call, const llvm::Instruction *return_site,
      llvm::ArrayRef<const llvm::Function *> callees, const Fact &src_fact,
      const Fact &tgt_fact) override;

  // Configuration
  std::shared_ptr<TypeStateProperty> get_property() const { return m_property; }

  // Tracked values configuration
  void track_globals(bool enable) { m_track_globals = enable; }
  void track_heap(bool enable) { m_track_heap = enable; }
  void add_tracked_type(const std::string &type_name) {
    m_tracked_types.insert(type_name);
  }

  // Query interface
  bool is_error_state(const Value &v) const {
    return !v.is_special() && m_property->is_error_state(v.user_state());
  }

private:
  std::shared_ptr<TypeStateProperty> m_property;
  bool m_track_globals;
  bool m_track_heap;
  std::unordered_set<std::string> m_tracked_types;

  // Helper: should we track this value?
  bool should_track(const llvm::Value *val) const;

  // Helper: get edge function for transition
  EdgeFunction make_transition_function(
      const std::vector<TypeStateProperty::Transition> &transitions) const;
};

// ============================================================================
// Predefined Typestate Properties
// ============================================================================

namespace predefined {
std::shared_ptr<TypeStateProperty> create_file_property();
std::shared_ptr<TypeStateProperty> create_lock_property();
std::shared_ptr<TypeStateProperty> create_memory_property();
std::shared_ptr<TypeStateProperty> create_socket_property();
} // namespace predefined

} // namespace ifds
