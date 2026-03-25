#ifndef CHECKER_PULSE_PULSETAINT_H
#define CHECKER_PULSE_PULSETAINT_H

#include "Checker/Pulse/Core/PulseAbstractValue.h"
#include "Checker/Pulse/Core/PulseValueHistory.h"
#include "Checker/Pulse/Domain/PulseTaintConfig.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

namespace pulse {

class AbductiveDomain;

// TaintKind is now defined in PulseTaintConfig.h

/**
 * TaintOrigin: where the taint originated from
 * Aligned with Infer's TaintItem.origin
 */
enum class TaintOrigin {
  Argument,          // From function argument
  InstanceReference, // From instance reference (this/self)
  ReturnValue,       // From return value
  Allocation,        // From allocation
  GetField,          // From field read
  SetField,          // From field write
  FieldOfValue       // From field of a value
};

/**
 * TaintValue: what is tainted
 * Aligned with Infer's TaintItem.value
 */
struct TaintValue {
  enum class Type {
    TaintBlockPassedTo, // Block/closure passed to function
    TaintField,         // Field is tainted
    TaintProcedure      // Procedure is tainted
  };

  Type type;
  std::string procedure_name; // For TaintProcedure
  std::string field_name;     // For TaintField

  TaintValue(Type t) : type(t) {}
  TaintValue(Type t, const std::string &name) : type(t), procedure_name(name) {}
};

/**
 * TaintValueTuple: structured representation of what is tainted
 * Aligned with Infer's TaintItem.value_tuple
 */
struct TaintValueTuple {
  enum class Type {
    Basic,      // Basic value with origin
    FieldOf,    // Field of a value tuple
    PointedToBy // Pointed to by a value tuple
  };

  Type type;
  TaintValue value;
  TaintOrigin origin;
  std::unique_ptr<TaintValueTuple> nested; // For FieldOf and PointedToBy

  TaintValueTuple(const TaintValue &v, TaintOrigin o)
      : type(Type::Basic), value(v), origin(o), nested(nullptr) {}

  // Copy constructor
  TaintValueTuple(const TaintValueTuple &other)
      : type(other.type), value(other.value), origin(other.origin),
        nested(other.nested ? std::make_unique<TaintValueTuple>(*other.nested)
                            : nullptr) {}

  // Move constructor
  TaintValueTuple(TaintValueTuple &&) noexcept = default;

  // Copy assignment
  TaintValueTuple &operator=(const TaintValueTuple &other) {
    if (this != &other) {
      type = other.type;
      value = other.value;
      origin = other.origin;
      nested = other.nested ? std::make_unique<TaintValueTuple>(*other.nested)
                            : nullptr;
    }
    return *this;
  }

  // Move assignment
  TaintValueTuple &operator=(TaintValueTuple &&) noexcept = default;

  // Destructor
  ~TaintValueTuple() = default;
};

/**
 * Taint item: represents a specific instance of taint on a value
 * Enhanced to align with Infer's TaintItem structure
 */
struct TaintItem {
  std::vector<TaintKind> kinds; // Multiple kinds can apply
  TaintValueTuple value_tuple;  // Structured representation
  const llvm::Instruction *source_instruction;
  const llvm::Function *source_function;
  ValueHistory history;       // How the taint propagated to current value
  unsigned timestamp;         // When taint was introduced
  bool intra_procedural_only; // True if taint is only intra-procedural

  // Sanitizer information
  struct Sanitizer {
    TaintKind sanitizer_kind;
    const llvm::Instruction *sanitizer_location;
    unsigned sanitizer_timestamp;

    Sanitizer(TaintKind k, const llvm::Instruction *loc, unsigned ts)
        : sanitizer_kind(std::move(k)), sanitizer_location(loc),
          sanitizer_timestamp(ts) {}
  };
  std::vector<Sanitizer> sanitizers; // Sanitizers that have been applied

  TaintItem(const std::vector<TaintKind> &k, const TaintValueTuple &vt,
            const llvm::Instruction *inst, const llvm::Function *func,
            unsigned ts = 0)
      : kinds(k), value_tuple(vt), source_instruction(inst),
        source_function(func), timestamp(ts), intra_procedural_only(false) {}

  // Legacy constructor for backward compatibility
  TaintItem(const TaintKind &k, const llvm::Instruction *inst,
            const llvm::Function *func, unsigned ts = 0)
      : kinds({k}), value_tuple(TaintValueTuple(
                        TaintValue(TaintValue::Type::TaintProcedure,
                                   func ? func->getName().str() : ""),
                        TaintOrigin::ReturnValue)),
        source_instruction(inst), source_function(func), timestamp(ts),
        intra_procedural_only(false) {}

  // Copy constructor
  TaintItem(const TaintItem &other)
      : kinds(other.kinds), value_tuple(other.value_tuple),
        source_instruction(other.source_instruction),
        source_function(other.source_function), history(other.history),
        timestamp(other.timestamp),
        intra_procedural_only(other.intra_procedural_only),
        sanitizers(other.sanitizers) {}

  // Move constructor
  TaintItem(TaintItem &&) noexcept = default;

  // Copy assignment
  TaintItem &operator=(const TaintItem &other) {
    if (this != &other) {
      kinds = other.kinds;
      value_tuple = other.value_tuple;
      source_instruction = other.source_instruction;
      source_function = other.source_function;
      history = other.history;
      timestamp = other.timestamp;
      intra_procedural_only = other.intra_procedural_only;
      sanitizers = other.sanitizers;
    }
    return *this;
  }

  // Move assignment
  TaintItem &operator=(TaintItem &&) noexcept = default;

  // Destructor
  ~TaintItem() = default;

  bool operator<(const TaintItem &other) const {
    if (kinds != other.kinds)
      return kinds < other.kinds;
    if (source_instruction != other.source_instruction)
      return source_instruction < other.source_instruction;
    if (timestamp != other.timestamp)
      return timestamp < other.timestamp;
    return false;
  }

  /**
   * Check if this taint item is sanitized by any sanitizer
   */
  bool isSanitized() const { return !sanitizers.empty(); }

  /**
   * Check if sanitized by specific kinds
   */
  bool isSanitizedBy(const std::vector<TaintKind> &sanitizer_kinds) const {
    for (const auto &sanitizer : sanitizers) {
      for (const auto &kind : sanitizer_kinds) {
        if (sanitizer.sanitizer_kind == kind) {
          return true;
        }
      }
    }
    return false;
  }

  /**
   * Add a sanitizer to this taint item
   */
  void addSanitizer(TaintKind sanitizer_kind, const llvm::Instruction *loc,
                    unsigned ts) {
    sanitizers.emplace_back(sanitizer_kind, loc, ts);
  }

  /**
   * Get primary kind (first kind, or Unknown if empty)
   */
  TaintKind getPrimaryKind() const {
    return kinds.empty() ? TaintKind::Unknown() : kinds[0];
  }
};

/**
 * Taint domain: maps abstract values to their taint items
 */
class TaintDomain {
private:
  std::map<AbstractValue, std::set<TaintItem>> taints_;

public:
  void add(AbstractValue v, TaintItem item);
  void remove(AbstractValue v);
  bool has(AbstractValue v) const;
  const std::set<TaintItem> &get(AbstractValue v) const;

  // Merge two taint domains
  void join(const TaintDomain &other);

  const std::map<AbstractValue, std::set<TaintItem>> &getMap() const {
    return taints_;
  }
};

/**
 * Taint operations: high-level taint analysis logic
 * Enhanced to use TaintConfig and align with Infer's PulseTaintOperations
 */
class TaintOperations {
private:
  static unsigned global_timestamp_; // Global timestamp counter
  static const TaintConfig *config_; // Taint configuration

public:
  /**
   * Set taint configuration
   */
  static void setConfig(const TaintConfig *config) { config_ = config; }

  /**
   * Get taint configuration (returns default if not set)
   */
  static const TaintConfig &getConfig() {
    if (!config_) {
      return TaintConfig::getDefault();
    }
    return *config_;
  }
  /**
   * Mark a value as tainted
   */
  static void taint(AbductiveDomain &astate, AbstractValue v,
                    const std::vector<TaintKind> &kinds,
                    const llvm::Instruction *source);

  /**
   * Mark a value as tainted (legacy single-kind version)
   */
  static void taint(AbductiveDomain &astate, AbstractValue v, TaintKind kind,
                    const llvm::Instruction *source) {
    taint(astate, v, {std::move(kind)}, source);
  }

  /**
   * Mark a value as tainted using procedure name only (avoids dereferencing
   * the instruction; use when source may be invalid or to avoid
   * EXC_BAD_ACCESS).
   */
  static void taint(AbductiveDomain &astate, AbstractValue v, TaintKind kind,
                    const std::string &procedure_name);

  /**
   * Check if a value is tainted and report if it flows to a sink
   * Returns true if a bug was reported
   */
  static bool checkSink(AbductiveDomain &astate, AbstractValue v,
                        const std::string &sink_name,
                        const llvm::Instruction *sink_loc);

  /**
   * Propagate taint from source value to dest value
   */
  static void propagate(AbductiveDomain &astate, AbstractValue src,
                        AbstractValue dest, const llvm::Instruction *loc);

  /**
   * Sanitize a tainted value: mark it as sanitized
   */
  static void sanitize(AbductiveDomain &astate, AbstractValue v,
                       TaintKind sanitizer_kind,
                       const llvm::Instruction *sanitizer_loc);

  /**
   * Check if a value is sanitized
   */
  static bool isSanitized(const AbductiveDomain &astate, AbstractValue v);

  /**
   * Propagate taint through memory operations (load/store)
   */
  static void propagateThroughLoad(AbductiveDomain &astate,
                                   AbstractValue src_addr,
                                   AbstractValue dest_val,
                                   const llvm::Instruction *loc);

  static void propagateThroughStore(AbductiveDomain &astate,
                                    AbstractValue src_val,
                                    AbstractValue dest_addr,
                                    const llvm::Instruction *loc);

  /**
   * Propagate taint through function calls
   */
  static void propagateThroughCall(AbductiveDomain &astate,
                                   const llvm::CallInst *call,
                                   const std::vector<AbstractValue> &args,
                                   AbstractValue ret_val);

  /**
   * Get next timestamp
   */
  static unsigned getNextTimestamp() { return global_timestamp_++; }

  /**
   * Reset timestamp counter
   */
  static void resetTimestamp() { global_timestamp_ = 1; }
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSETAINT_H
