#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"

namespace ifds {

struct InstInteractionValue {
  enum Kind { Bottom, None, Read, Write, ReadWrite, Top } kind;
  InstInteractionValue() : kind(Bottom) {}
  explicit InstInteractionValue(Kind k) : kind(k) {}

  static InstInteractionValue bottom() { return InstInteractionValue(Bottom); }
  static InstInteractionValue top() { return InstInteractionValue(Top); }
  static InstInteractionValue none() { return InstInteractionValue(None); }
  static InstInteractionValue read() { return InstInteractionValue(Read); }
  static InstInteractionValue write() { return InstInteractionValue(Write); }
  static InstInteractionValue read_write() {
    return InstInteractionValue(ReadWrite);
  }

  bool operator==(const InstInteractionValue &other) const {
    return kind == other.kind;
  }
};

class IDEInstInteractionAnalysis
    : public DefaultNoAliasIDEProblem<const llvm::Value *,
                                      InstInteractionValue> {
public:
  using Fact = const llvm::Value *;
  using Value = InstInteractionValue;

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
};

} // namespace ifds
