#ifndef CHECKER_PULSE_PULSEVALUEHISTORY_H
#define CHECKER_PULSE_PULSEVALUEHISTORY_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

namespace llvm {
class Instruction;
class Function;
class Value;
} // namespace llvm

namespace pulse {

class AbstractValue;

/**
 * ValueHistory: tracks how a value was derived
 * Records allocation sites, function calls, and other events
 * Implemented as a persistent linked list (shared structure)
 */
class ValueHistory {
public:
  enum class EventKind {
    Allocation,          // Value allocated (malloc, new, etc.)
    FunctionCall,        // Value returned from function call
    Load,                // Value loaded from memory
    Store,               // Value stored to memory
    Parameter,           // Value is a function parameter
    Return,              // Value is a return value
    VariableDeclaration, // Variable declared
    Conditional,         // Path condition (if/else)
    Unknown              // Unknown origin
  };

  struct Event {
    EventKind kind;
    const llvm::Instruction *location; // Where the event occurred
    const llvm::Function *function;    // Function where event occurred
    AbstractValue
        *related_value;      // Related abstract value (for allocations, etc.)
    std::string description; // Optional extra description

    Event(EventKind k, const llvm::Instruction *loc,
          const llvm::Function *func = nullptr, std::string desc = "")
        : kind(k), location(loc), function(func), related_value(nullptr),
          description(std::move(desc)) {}
  };

private:
  struct Node {
    Event event;
    std::shared_ptr<const Node> parent;

    Node(Event e, std::shared_ptr<const Node> p)
        : event(std::move(e)), parent(std::move(p)) {}
  };

  std::shared_ptr<const Node> head_;

public:
  ValueHistory() = default;

  // Copy constructor shares the history (cheap)
  ValueHistory(const ValueHistory &) = default;
  ValueHistory &operator=(const ValueHistory &) = default;
  ValueHistory(ValueHistory &&) = default;
  ValueHistory &operator=(ValueHistory &&) = default;

  void addEvent(EventKind kind, const llvm::Instruction *loc,
                const llvm::Function *func = nullptr, std::string desc = "") {
    Event e(kind, loc, func, std::move(desc));
    head_ = std::make_shared<Node>(std::move(e), head_);
  }

  void addAllocationEvent(const llvm::Instruction *loc, AbstractValue *av) {
    Event e(EventKind::Allocation, loc);
    e.related_value = av;
    head_ = std::make_shared<Node>(std::move(e), head_);
  }

  // Get events in chronological order (oldest to newest)
  std::vector<Event> getEvents() const {
    std::vector<Event> events;
    for (auto node = head_; node; node = node->parent) {
      events.push_back(node->event);
    }
    // The list is newest-first, so reverse to get chronological order
    std::reverse(events.begin(), events.end());
    return events;
  }

  bool isEmpty() const { return !head_; }

  ValueHistory clone() const {
    return *this; // Sharing is caring
  }
};

/**
 * Trace: represents a trace of events leading to an error
 * Used for error reporting with full provenance
 */
class Trace {
public:
  struct TraceEvent {
    const llvm::Instruction *location;
    const llvm::Function *function;
    std::string description;

    TraceEvent(const llvm::Instruction *loc, const llvm::Function *func,
               const std::string &desc)
        : location(loc), function(func), description(desc) {}

    // Explicit copy constructor and assignment operator
    TraceEvent(const TraceEvent &) = default;
    TraceEvent &operator=(const TraceEvent &) = default;
    TraceEvent(TraceEvent &&) = default;
    TraceEvent &operator=(TraceEvent &&) = default;
    ~TraceEvent() = default;
  };

private:
  std::vector<TraceEvent> events_;

public:
  Trace() = default;

  void addEvent(const llvm::Instruction *loc, const llvm::Function *func,
                const std::string &desc) {
    events_.emplace_back(loc, func, desc);
  }

  void addEvent(const llvm::Instruction *loc, const std::string &desc) {
    if (loc) {
      addEvent(loc, loc->getFunction(), desc);
    }
  }

  const std::vector<TraceEvent> &getEvents() const { return events_; }
  bool isEmpty() const { return events_.empty(); }

  /** Build a trace from instruction history (for error reporting). */
  static Trace fromInstructionHistory(
      const std::vector<const llvm::Instruction *> &history) {
    Trace t;
    for (const llvm::Instruction *i : history) {
      if (!i)
        continue;
      std::string desc = i->getOpcodeName();
      if (const llvm::Function *f = i->getFunction()) {
        std::string prefix = f->getName().str();
        prefix += ": ";
        prefix += desc;
        desc = std::move(prefix);
      }
      t.addEvent(i, desc);
    }
    return t;
  }

  /** Build a trace from ValueHistory. */
  static Trace fromValueHistory(const ValueHistory &history) {
    Trace t;
    for (const auto &event : history.getEvents()) {
      if (!event.location)
        continue;
      std::string desc;
      switch (event.kind) {
      case ValueHistory::EventKind::Allocation:
        desc = "Allocated";
        break;
      case ValueHistory::EventKind::FunctionCall:
        desc = "Returned from call";
        break;
      case ValueHistory::EventKind::Load:
        desc = "Loaded from memory";
        break;
      case ValueHistory::EventKind::Store:
        desc = "Stored to memory";
        break;
      case ValueHistory::EventKind::Parameter:
        desc = "Parameter";
        break;
      case ValueHistory::EventKind::Return:
        desc = "Return value";
        break;
      default:
        desc = "Event";
        break;
      }
      if (event.function) {
        std::string prefix = event.function->getName().str();
        prefix += ": ";
        prefix += desc;
        desc = std::move(prefix);
      }
      t.addEvent(event.location, desc);
    }
    return t;
  }

  /** Build a trace from ValueHistory, filtering to only include null constant
   * sources. */
  static Trace fromValueHistoryNullSourceOnly(const ValueHistory &history) {
    Trace t;
    auto is_null_ptr_const = [](const llvm::Value *v) -> bool {
      if (!v)
        return false;
      if (llvm::isa<llvm::ConstantPointerNull>(v))
        return true;
      if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(v)) {
        if (CE->getOpcode() == llvm::Instruction::IntToPtr) {
          if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(CE->getOperand(0))) {
            return CI->isZero();
          }
        }
      }
      if (auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(v)) {
        if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(I2P->getOperand(0))) {
          return CI->isZero();
        }
      }
      return false;
    };
    for (const auto &event : history.getEvents()) {
      if (!event.location)
        continue;

      // Only include Store events where a null constant is stored
      if (event.kind == ValueHistory::EventKind::Store) {
        if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(event.location)) {
          const llvm::Value *stored_value = SI->getValueOperand();
          // Check if storing a null pointer constant (not integer 0).
          if (is_null_ptr_const(stored_value)) {
            std::string desc = "Null constant stored";
            if (event.function) {
              std::string prefix = event.function->getName().str();
              prefix += ": ";
              prefix += desc;
              desc = std::move(prefix);
            }
            t.addEvent(event.location, desc);
          }
        }
      }
    }
    return t;
  }

  /** Maximum events to copy when cloning (avoids std::length_error and OOM on
   * huge traces). */
  static constexpr size_t kMaxTraceEventsForClone = 4096;

  Trace clone() const {
    Trace cloned;
    const size_t n = events_.size();
    if (n <= kMaxTraceEventsForClone) {
      cloned.events_ = events_;
    } else {
      // Keep the most recent events for error reporting
      const size_t skip = n - kMaxTraceEventsForClone;
      cloned.events_.reserve(kMaxTraceEventsForClone);
      for (size_t i = skip; i < n; ++i)
        cloned.events_.push_back(events_[i]);
    }
    return cloned;
  }
};

/**
 * ValueOrigin: tracks the source/origin of a value
 * Combines abstract value with its history
 */
class ValueOrigin {
public:
  enum class OriginKind {
    Stack,     // Value from stack variable
    Heap,      // Value from heap allocation
    Parameter, // Function parameter
    Return,    // Function return value
    Constant,  // Constant value
    Unknown    // Unknown origin
  };

private:
  AbstractValue *value_;
  ValueHistory history_;
  OriginKind kind_;
  const llvm::Instruction *origin_location_;

public:
  ValueOrigin(AbstractValue *av, OriginKind k,
              const llvm::Instruction *loc = nullptr)
      : value_(av), kind_(k), origin_location_(loc) {}

  AbstractValue *getValue() const { return value_; }
  const ValueHistory &getHistory() const { return history_; }
  ValueHistory &getHistory() { return history_; }
  OriginKind getKind() const { return kind_; }
  const llvm::Instruction *getOriginLocation() const {
    return origin_location_;
  }

  void addHistoryEvent(ValueHistory::EventKind kind,
                       const llvm::Instruction *loc,
                       const llvm::Function *func = nullptr) {
    history_.addEvent(kind, loc, func);
  }

  ValueOrigin clone() const {
    ValueOrigin cloned(value_, kind_, origin_location_);
    cloned.history_ = history_.clone();
    return cloned;
  }
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSEVALUEHISTORY_H
