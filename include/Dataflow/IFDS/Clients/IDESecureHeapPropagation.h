#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"

#include <string>
#include <unordered_set>

namespace ifds {

struct SecureHeapValue {
  enum Kind { Bottom, Unknown, Allocated, Secured, Freed, Error, Top } kind;
  SecureHeapValue() : kind(Bottom) {}
  explicit SecureHeapValue(Kind k) : kind(k) {}

  static SecureHeapValue bottom() { return SecureHeapValue(Bottom); }
  static SecureHeapValue top() { return SecureHeapValue(Top); }
  static SecureHeapValue unknown() { return SecureHeapValue(Unknown); }
  static SecureHeapValue allocated() { return SecureHeapValue(Allocated); }
  static SecureHeapValue secured() { return SecureHeapValue(Secured); }
  static SecureHeapValue freed() { return SecureHeapValue(Freed); }
  static SecureHeapValue error() { return SecureHeapValue(Error); }

  bool operator==(const SecureHeapValue &other) const {
    return kind == other.kind;
  }
};

class IDESecureHeapPropagation
    : public DefaultNoAliasIDEProblem<const llvm::Value *, SecureHeapValue> {
public:
  using Fact = const llvm::Value *;
  using Value = SecureHeapValue;

  IDESecureHeapPropagation();

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

  Value top_value() const override { return Value::top(); }
  Value bottom_value() const override { return Value::bottom(); }
  Value join(const Value &v1, const Value &v2) const override;

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

  FactSet summary_flow(const llvm::CallBase *call, const llvm::Function *callee,
                       const Fact &fact) override;
  EdgeFunction summary_edge_function(const llvm::CallBase *call,
                                     const llvm::Function *callee,
                                     const llvm::Instruction *return_site,
                                     const Fact &src_fact,
                                     const Fact &tgt_fact) override;

private:
  std::unordered_set<std::string> m_allocators;
  std::unordered_set<std::string> m_releasers;
  std::unordered_set<std::string> m_securers;
};

} // namespace ifds
