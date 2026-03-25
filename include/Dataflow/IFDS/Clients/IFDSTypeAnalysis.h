/*
 * IFDS Type Analysis
 *
 * Tracks value-level type reachability through assignments and calls.
 */

#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"

#include <map>

namespace ifds {

class TypeAnalysis : public DefaultNoAliasIFDSProblem<const llvm::Value *> {
public:
  using Fact = const llvm::Value *;

  Fact zero_fact() const override { return nullptr; }
  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ,
                      const Fact &fact) override;
  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const Fact &fact) override;
  FactSet return_flow(const llvm::CallBase *call, const llvm::Instruction *exit_inst, const llvm::Instruction *return_site, const llvm::Function *callee,
                      const Fact &exit_fact, const Fact &call_fact) override;
  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> callees,
                              const Fact &fact) override;
  FactSet initial_facts(const llvm::Function *main) override;

  std::map<const llvm::Value *, const llvm::Type *> infer_types(
      const std::unordered_map<const llvm::Instruction *, FactSet> &facts)
      const;
};

} // namespace ifds
