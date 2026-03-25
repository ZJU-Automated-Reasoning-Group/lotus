//===- AEDetector.h -- Vulnerability Detectors-----------------------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Checker/AE/AbstractState.h"

#include <cstddef>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

namespace lotus {
namespace analysis {

class AbstractInterpretation;
class AEExtAPI;

/// Event types for bug diagnosis
enum class AEBugEventType {
  ALLOC,   ///< Memory allocation
  FREE,    ///< Memory deallocation
  LOAD,    ///< Memory load
  STORE,   ///< Memory store
  DEREF,   ///< Pointer dereference
  COMPARE, ///< Pointer comparison
  BRANCH,  ///< Branch instruction
  CALL,    ///< Function call
  RETURN   ///< Function return
};

/// A single event in the bug diagnosis trace
struct AEBugEvent {
  AEBugEventType type;
  const llvm::Instruction *inst;
  std::string description;
  std::string srcFile;
  unsigned srcLine;
  unsigned srcCol;
  std::string funcName;

  AEBugEvent(AEBugEventType t, const llvm::Instruction *I,
             const std::string &desc)
      : type(t), inst(I), description(desc), srcLine(0), srcCol(0) {
    if (inst) {
      if (const llvm::DILocation *loc = inst->getDebugLoc()) {
        srcFile = loc->getFilename().str();
        srcLine = loc->getLine();
        srcCol = loc->getColumn();
      }
      if (const llvm::Function *func = inst->getFunction()) {
        funcName = func->getName().str();
      }
    }
  }
};

/// Base class for all detectors
class AEDetector {
public:
  enum DetectorKind {
    BUF_OVERFLOW,
    NULL_DEREF,
    DIV_ZERO,
    INT_OVERFLOW,
    USE_AFTER_FREE,
    INVALID_FREE,
    MEMORY_LEAK,
    UNKNOWN
  };

  AEDetector() : kind(UNKNOWN) {}
  virtual ~AEDetector() = default;

  static bool classof(const AEDetector *detector) {
    return detector->getKind() == AEDetector::UNKNOWN;
  }

  virtual void detect(AbstractState &as, const llvm::Instruction *inst) = 0;
  virtual void handleStubFunctions(const llvm::CallBase *call) = 0;
  virtual void reportBug() = 0;
  virtual size_t getBugCount() const = 0;
  virtual void reset() { clearEventTrace(); }

  DetectorKind getKind() const { return kind; }

protected:
  void addEventToTrace(AEBugEventType type, const llvm::Instruction *inst,
                       const std::string &desc);
  void clearEventTrace();
  std::vector<AEBugEvent> getEventTrace() const { return eventTrace; }

  DetectorKind kind;
  std::vector<AEBugEvent> eventTrace;
};

/// Exception class for handling errors in Abstract Execution
class AEException : public std::exception {
public:
  AEException(const std::string &message) : msg_(message) {}

  virtual const char *what() const throw() { return msg_.c_str(); }

private:
  std::string msg_;
};

/// Detector for identifying buffer overflow issues
class BufOverflowDetector : public AEDetector {
  friend class AbstractInterpretation;

public:
  BufOverflowDetector() {
    kind = BUF_OVERFLOW;
    initExtAPIBufOverflowCheckRules();
  }
  ~BufOverflowDetector() = default;

  static bool classof(const AEDetector *detector) {
    return detector->getKind() == AEDetector::BUF_OVERFLOW;
  }

  void detect(AbstractState &as, const llvm::Instruction *inst) override;
  void handleStubFunctions(const llvm::CallBase *call) override;
  void reportBug() override;
  size_t getBugCount() const override { return instToBugInfo.size(); }
  void reset() override;

  void detectExtAPI(AbstractState &as, const llvm::CallBase *call,
                    const llvm::Function *callee = nullptr);
  bool canSafelyAccessMemory(AbstractState &as, uint32_t ptrId,
                             const IntervalValue &len);
  IntervalValue getAccessOffset(AbstractState &as, uint32_t objId,
                                const llvm::GetElementPtrInst *gep);
  void updateGepObjOffsetFromBase(AbstractState &as, AddressValue gepAddrs,
                                  AddressValue objAddrs, IntervalValue offset);
  void addToGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep,
                                 const IntervalValue &offset);
  bool hasGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep) const;
  IntervalValue
  getGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep) const;

private:
  void initExtAPIBufOverflowCheckRules();
  bool detectStrcpy(AbstractState &as, const llvm::CallBase *call,
                    const llvm::Function *callee = nullptr);
  bool detectStrcat(AbstractState &as, const llvm::CallBase *call,
                    const llvm::Function *callee = nullptr);
  void addBugToReporter(const AEException &e, const llvm::Instruction *inst);

  std::map<std::string, std::vector<std::pair<uint32_t, uint32_t>>>
      extAPIBufOverflowCheckRules;
  std::set<std::string> bugLoc;
  std::map<const llvm::Instruction *, std::string> instToBugInfo;
  std::map<const llvm::GetElementPtrInst *, IntervalValue> gepObjOffsetFromBase;
  std::map<uint32_t, IntervalValue> gepObjOffsetFromBaseByObjId;
};

/// Detector for identifying null pointer dereference issues
class NullptrDerefDetector : public AEDetector {
  friend class AbstractInterpretation;

public:
  NullptrDerefDetector() { kind = NULL_DEREF; }
  ~NullptrDerefDetector() = default;

  static bool classof(const AEDetector *detector) {
    return detector->getKind() == AEDetector::NULL_DEREF;
  }

  void detect(AbstractState &as, const llvm::Instruction *inst) override;
  void handleStubFunctions(const llvm::CallBase *call) override;
  void reportBug() override;
  size_t getBugCount() const override { return instToBugInfo.size(); }
  void reset() override;

  void detectExtAPI(AbstractState &as, const llvm::CallBase *call,
                    const llvm::Function *callee = nullptr);
  bool canSafelyDerefPtr(AbstractState &as, uint32_t ptrId);

  bool isUninit(const AbstractValue &v) {
    return v.getAddrs().isBottom() && v.getInterval().isBottom();
  }

  bool isNull(const AbstractValue &v) { return !v.isAddr() && !v.isInterval(); }

private:
  void addBugToReporter(const AEException &e, const llvm::Instruction *inst);

  std::set<std::string> bugLoc;
  std::map<const llvm::Instruction *, std::string> instToBugInfo;
};

/// Detector for divide-by-zero operations and checkpoints.
class DivZeroDetector : public AEDetector {
  friend class AbstractInterpretation;

public:
  DivZeroDetector() { kind = DIV_ZERO; }
  ~DivZeroDetector() = default;

  static bool classof(const AEDetector *detector) {
    return detector->getKind() == AEDetector::DIV_ZERO;
  }

  void detect(AbstractState &as, const llvm::Instruction *inst) override;
  void handleStubFunctions(const llvm::CallBase *call) override;
  void reportBug() override;
  size_t getBugCount() const override { return instToBugInfo.size(); }
  void reset() override;

private:
  void addBugToReporter(const AEException &e, const llvm::Instruction *inst);
  bool mayDivideByZero(AbstractState &as, const llvm::Value *divisor) const;

  std::set<std::string> bugLoc;
  std::map<const llvm::Instruction *, std::string> instToBugInfo;
};

/// Detector for integer-overflow operations and checkpoints.
class OverflowDetector : public AEDetector {
  friend class AbstractInterpretation;

public:
  OverflowDetector() { kind = INT_OVERFLOW; }
  ~OverflowDetector() = default;

  static bool classof(const AEDetector *detector) {
    return detector->getKind() == AEDetector::INT_OVERFLOW;
  }

  void detect(AbstractState &as, const llvm::Instruction *inst) override;
  void handleStubFunctions(const llvm::CallBase *call) override;
  void reportBug() override;
  size_t getBugCount() const override { return instToBugInfo.size(); }
  void reset() override;

private:
  void addBugToReporter(const AEException &e, const llvm::Instruction *inst);
  bool mayOverflow(AbstractState &as, const llvm::Instruction *inst) const;

  std::set<std::string> bugLoc;
  std::map<const llvm::Instruction *, std::string> instToBugInfo;
};

/// Detector for identifying use-after-free issues
class UseAfterFreeDetector : public AEDetector {
  friend class AbstractInterpretation;

public:
  UseAfterFreeDetector() { kind = USE_AFTER_FREE; }
  ~UseAfterFreeDetector() = default;

  static bool classof(const AEDetector *detector) {
    return detector->getKind() == AEDetector::USE_AFTER_FREE;
  }

  void detect(AbstractState &as, const llvm::Instruction *inst) override;
  void handleStubFunctions(const llvm::CallBase *call) override;
  void reportBug() override;
  size_t getBugCount() const override { return instToBugInfo.size(); }
  void reset() override;

  bool mayAccessFreedMem(AbstractState &as, uint32_t ptrId);

private:
  void addBugToReporter(const AEException &e, const llvm::Instruction *inst);

  std::set<std::string> bugLoc;
  std::map<const llvm::Instruction *, std::string> instToBugInfo;
};

/// Detector for identifying invalid free issues (double-free, free non-heap)
class InvalidFreeDetector : public AEDetector {
  friend class AbstractInterpretation;

public:
  InvalidFreeDetector() { kind = INVALID_FREE; }
  ~InvalidFreeDetector() = default;

  static bool classof(const AEDetector *detector) {
    return detector->getKind() == AEDetector::INVALID_FREE;
  }

  void detect(AbstractState &as, const llvm::Instruction *inst) override;
  void handleStubFunctions(const llvm::CallBase *call) override;
  void reportBug() override;
  size_t getBugCount() const override { return instToBugInfo.size(); }
  void reset() override;

  bool isValidFree(AbstractState &as, uint32_t ptrId);

private:
  void addBugToReporter(const AEException &e, const llvm::Instruction *inst);

  std::set<std::string> bugLoc;
  std::map<const llvm::Instruction *, std::string> instToBugInfo;
};

/// Detector for identifying memory leaks (allocated but not freed)
class MemLeakDetector : public AEDetector {
  friend class AbstractInterpretation;

public:
  MemLeakDetector() { kind = MEMORY_LEAK; }
  ~MemLeakDetector() = default;

  static bool classof(const AEDetector *detector) {
    return detector->getKind() == AEDetector::MEMORY_LEAK;
  }

  void detect(AbstractState &as, const llvm::Instruction *inst) override;
  void handleStubFunctions(const llvm::CallBase *call) override;
  void reportBug() override;
  size_t getBugCount() const override { return instToBugInfo.size(); }
  void reset() override;

  /// Track allocations throughout the program
  void trackAllocation(uint32_t objId, const llvm::Instruction *allocSite);

  /// Check if an object is reachable from live pointers
  bool isReachableFromLivePointers(AbstractState &as, uint32_t objId);

  /// Check if an object escapes (stored to global, returned, passed to external
  /// function)
  bool objectEscapes(uint32_t objId);

private:
  void addBugToReporter(const AEException &e, const llvm::Instruction *inst);

  /// Map from object ID to allocation site
  std::map<uint32_t, const llvm::Instruction *> objToAllocSite;

  /// Set of objects that escape (may be freed elsewhere)
  std::set<uint32_t> escapedObjects;

  /// Reported bugs
  std::set<std::string> bugLoc;
  std::map<const llvm::Instruction *, std::string> instToBugInfo;
};

} // namespace analysis
} // namespace lotus
