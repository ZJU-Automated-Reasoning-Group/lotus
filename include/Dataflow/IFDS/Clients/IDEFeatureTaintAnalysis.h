#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ifds {

struct FeatureTaintValue {
  enum Kind { Bottom, Features, Top } kind;
  uint64_t mask;

  FeatureTaintValue() : kind(Bottom), mask(0) {}
  FeatureTaintValue(Kind k, uint64_t m) : kind(k), mask(m) {}

  static FeatureTaintValue bottom() { return FeatureTaintValue(Bottom, 0); }
  static FeatureTaintValue top() { return FeatureTaintValue(Top, 0); }
  static FeatureTaintValue features(uint64_t m) {
    return FeatureTaintValue(Features, m);
  }

  bool operator==(const FeatureTaintValue &other) const {
    return kind == other.kind && mask == other.mask;
  }
};

class IDEFeatureTaintAnalysis
    : public DefaultNoAliasIDEProblem<const llvm::Value *, FeatureTaintValue> {
public:
  using Fact = const llvm::Value *;
  using Value = FeatureTaintValue;

  IDEFeatureTaintAnalysis();

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
  std::unordered_map<std::string, uint64_t> m_source_feature_bits;
  std::unordered_map<std::string, uint64_t> m_sanitizer_clears;
};

} // namespace ifds
