/*
 * Prototype IDE Analysis (ported from Phasar IDEProtoAnalysis)
 */

#include "Dataflow/IFDS/Clients/IDEProtoAnalysis.h"

namespace ifds {

IDEProtoAnalysis::FactSet
IDEProtoAnalysis::normal_flow(const llvm::Instruction *,
                              const llvm::Instruction *, const Fact &fact) {
  FactSet out;
  out.insert(fact);
  return out;
}

IDEProtoAnalysis::FactSet IDEProtoAnalysis::call_flow(const llvm::CallBase *,
                                                      const llvm::Function *,
                                                      const Fact &fact) {
  FactSet out;
  out.insert(fact);
  return out;
}

IDEProtoAnalysis::FactSet
IDEProtoAnalysis::return_flow(const llvm::CallBase *,
                              const llvm::Instruction *,
                              const llvm::Instruction *,
                              const llvm::Function *, const Fact &,
                              const Fact &call_fact) {
  FactSet out;
  out.insert(call_fact);
  return out;
}

IDEProtoAnalysis::FactSet
IDEProtoAnalysis::call_to_return_flow(const llvm::CallBase *,
                                      const llvm::Instruction *,
                                      llvm::ArrayRef<const llvm::Function *>,
                                      const Fact &fact) {
  FactSet out;
  out.insert(fact);
  return out;
}

IDEProtoAnalysis::FactSet
IDEProtoAnalysis::initial_facts(const llvm::Function *) {
  FactSet out;
  out.insert(zero_fact());
  return out;
}

IDEProtoAnalysis::EdgeFunction
IDEProtoAnalysis::normal_edge_function(const llvm::Instruction *,
                                       const llvm::Instruction *,
                                       const Fact &,
                                       const Fact &) {
  return [](const Value &v) { return v; };
}

IDEProtoAnalysis::EdgeFunction
IDEProtoAnalysis::call_edge_function(const llvm::CallBase *,
                                     const llvm::Function *, const Fact &,
                                     const Fact &) {
  return [](const Value &v) { return v; };
}

IDEProtoAnalysis::EdgeFunction
IDEProtoAnalysis::return_edge_function(const llvm::CallBase *,
                                       const llvm::Function *,
                                       const llvm::Instruction *,
                                       const llvm::Instruction *,
                                       const Fact &, const Fact &) {
  return [](const Value &v) { return v; };
}

IDEProtoAnalysis::EdgeFunction
IDEProtoAnalysis::call_to_return_edge_function(const llvm::CallBase *,
                                               const llvm::Instruction *,
                                               llvm::ArrayRef<const llvm::Function *>,
                                               const Fact &, const Fact &) {
  return [](const Value &v) { return v; };
}

} // namespace ifds
