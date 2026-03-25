/*
 * IFDS Const Analysis
 *
 * Detects mutable memory locations (stack and heap).
 * A memory location is considered mutable after the second write access,
 * allowing for initial initialization.
 */

#pragma once

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Dataflow/IFDS/Core/IFDSFramework.h"

#include <set>

#include <llvm/IR/Instructions.h>

namespace ifds {

struct ConstFact {
  enum Type {
    ZERO,
    MUTABLE_MEMORY,    // Memory location that has been written multiple times
    INITIALIZED_MEMORY // Memory that has been initialized (first write)
  };

  Type type;
  const llvm::Value *value;

  ConstFact() : type(ZERO), value(nullptr) {}
  ConstFact(Type t, const llvm::Value *v) : type(t), value(v) {}

  static ConstFact zero() { return ConstFact(ZERO, nullptr); }
  static ConstFact mutable_mem(const llvm::Value *v) {
    return ConstFact(MUTABLE_MEMORY, v);
  }
  static ConstFact initialized(const llvm::Value *v) {
    return ConstFact(INITIALIZED_MEMORY, v);
  }

  bool operator==(const ConstFact &other) const {
    return type == other.type && value == other.value;
  }

  bool operator!=(const ConstFact &other) const { return !(*this == other); }

  bool operator<(const ConstFact &other) const {
    if (type != other.type)
      return type < other.type;
    return value < other.value;
  }

  bool is_zero() const { return type == ZERO; }
  bool is_mutable() const { return type == MUTABLE_MEMORY; }
  bool is_initialized() const { return type == INITIALIZED_MEMORY; }
};

} // namespace ifds

namespace std {
template <> struct hash<ifds::ConstFact> {
  size_t operator()(const ifds::ConstFact &fact) const {
    // FNV-1a-style mixing to avoid XOR-shift collisions on aligned hashes.
    size_t h = 14695981039346656037ULL;
    h ^= std::hash<int>{}(static_cast<int>(fact.type));
    h *= 1099511628211ULL;
    h ^= std::hash<const llvm::Value *>{}(fact.value);
    h *= 1099511628211ULL;
    return h;
  }
};
} // namespace std

namespace ifds {

class ConstAnalysis : public DefaultAliasAwareIFDSProblem<ConstFact> {
public:
  ConstAnalysis();
  explicit ConstAnalysis(lotus::AliasAnalysisWrapper *aa);

  ConstFact zero_fact() const override;
  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ,
                      const ConstFact &fact) override;
  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const ConstFact &fact) override;
  FactSet return_flow(const llvm::CallBase *call, const llvm::Instruction *exit_inst, const llvm::Instruction *return_site, const llvm::Function *callee,
                      const ConstFact &exit_fact,
                      const ConstFact &call_fact) override;
  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> callees, const ConstFact &fact) override;
  FactSet initial_facts(const llvm::Function *main) override;

  void set_alias_analysis(lotus::AliasAnalysisWrapper *aa) override;

  bool is_initialized(const llvm::Value *val) const;
  void mark_initialized(const llvm::Value *val);
  std::size_t initialized_count() const;

  void emit_report(llvm::raw_ostream &OS = llvm::outs()) const;

private:
  std::set<const llvm::Value *> initialized_locations;
  std::set<const llvm::Value *> all_memory_locations;

  bool is_vtable_store(const llvm::StoreInst *store) const;
  bool is_memory_intrinsic(const llvm::CallBase *call) const;
  std::set<const llvm::Value *>
  get_context_relevant_aliases(const std::set<const llvm::Value *> &aliases,
                               const llvm::Function *context) const;
};

} // namespace ifds
