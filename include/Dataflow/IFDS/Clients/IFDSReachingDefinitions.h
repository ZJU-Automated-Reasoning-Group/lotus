/*
 * Interprocedural Reaching Definitions Analysis using IFDS
 *
 * This implements reaching definitions analysis to demonstrate
 * another use case of the IFDS framework.
 */

#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

namespace ifds {

// ============================================================================
// Definition Fact
// ============================================================================

class DefinitionFact {
public:
  enum Type { ZERO, DEFINITION };

private:
  Type m_type;
  const llvm::Value *m_variable;
  const llvm::Instruction *m_definition_site;

public:
  DefinitionFact();

  static DefinitionFact zero();
  static DefinitionFact definition(const llvm::Value *var,
                                   const llvm::Instruction *def_site);

  bool operator==(const DefinitionFact &other) const;
  bool operator<(const DefinitionFact &other) const;
  bool operator!=(const DefinitionFact &other) const;

  Type get_type() const;
  const llvm::Value *get_variable() const;
  const llvm::Instruction *get_definition_site() const;

  bool is_zero() const;
  bool is_definition() const;

  friend std::ostream &operator<<(std::ostream &os, const DefinitionFact &fact);
};

} // namespace ifds

// Hash function for DefinitionFact
namespace std {
template <> struct hash<ifds::DefinitionFact> {
  size_t operator()(const ifds::DefinitionFact &fact) const;
};
} // namespace std

namespace ifds {

// ============================================================================
// Interprocedural Reaching Definitions Analysis
// ============================================================================

class ReachingDefinitionsAnalysis
    : public DefaultAliasAwareIFDSProblem<DefinitionFact> {
public:
  // IFDS interface implementation
  DefinitionFact zero_fact() const override;
  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ,
                      const DefinitionFact &fact) override;
  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const DefinitionFact &fact) override;
  FactSet return_flow(const llvm::CallBase *call, const llvm::Instruction *exit_inst, const llvm::Instruction *return_site, const llvm::Function *callee,
                      const DefinitionFact &exit_fact,
                      const DefinitionFact &call_fact) override;
  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> callees, const DefinitionFact &fact) override;
  FactSet initial_facts(const llvm::Function *main) override;

  // Query interface
  std::vector<const llvm::Instruction *>
  get_reaching_definitions(const llvm::Instruction *use_site,
                           const llvm::Value *variable) const;

private:
  bool defines_variable(const llvm::Instruction *inst) const;
  const llvm::Value *get_defined_variable(const llvm::Instruction *inst) const;
  bool is_local_to_caller(const DefinitionFact &fact,
                          const llvm::Function *callee) const;
  bool is_killed_by_external_call(const DefinitionFact &fact,
                                  const llvm::CallBase *call) const;
};

} // namespace ifds
