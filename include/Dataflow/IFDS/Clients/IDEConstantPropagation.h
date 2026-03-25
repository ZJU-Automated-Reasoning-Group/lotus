#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"

#include <llvm/ADT/Optional.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Operator.h>

namespace ifds {

// Linear Constant Propagation value lattice
struct LCPValue {
  enum Kind { Top, Const, Bottom } kind;
  long long value; // valid only when kind == Const

  LCPValue() : kind(Top), value(0) {}
  LCPValue(Kind k, long long v) : kind(k), value(v) {}
  static LCPValue top() { return LCPValue(Top, 0); }
  static LCPValue bottom() { return LCPValue(Bottom, 0); }
  static LCPValue constant(long long v) { return LCPValue(Const, v); }

  bool operator==(const LCPValue &other) const {
    if (kind != other.kind)
      return false;
    if (kind == Const)
      return value == other.value;
    return true;
  }
};

// IDE problem for linear constant propagation
class IDEConstantPropagation
    : public DefaultNoAliasIDEProblem<const llvm::Value *, LCPValue> {
public:
  using Fact = const llvm::Value *;
  using Value = LCPValue;

  // IFDS interface
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

  // Value domain
  Value top_value() const override { return Value::top(); }
  Value bottom_value() const override { return Value::bottom(); }
  Value join(const Value &v1, const Value &v2) const override;

  // Edge functions
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
  static bool definesValue(const llvm::Instruction *I);
  static const llvm::Value *getDefinedValue(const llvm::Instruction *I);
  static bool isCopy(const llvm::Instruction *I, const llvm::Value *&from);
  static llvm::Optional<long long> asConst(const llvm::Value *v);
  static llvm::Optional<long long> applyBinOp(unsigned opcode, long long a,
                                              long long b);
};

} // namespace ifds
