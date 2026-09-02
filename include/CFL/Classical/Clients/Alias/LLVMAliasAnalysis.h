#pragma once

#include "CFL/Classical/Clients/Alias/Alias.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class Module;
class Value;
} // namespace llvm

namespace lotus::cfl::classical {

struct LLVMAliasOptions {
  AliasEncodingMode encoding = AliasEncodingMode::PAG;
  SolverBackend backend = SolverBackend::SparseBitVector;
  std::string entry = "main";
  std::size_t max_callgraph_rounds = 64;
};

/// End-to-end LLVM alias analysis using Aser only as the constraint/model
/// frontend. Points-to propagation and indirect-call discovery are driven by
/// AliasClient's CFL relation rather than Aser's native points-to solver.
class LLVMCFLAliasAnalysis {
public:
  explicit LLVMCFLAliasAnalysis(LLVMAliasOptions options = {});
  ~LLVMCFLAliasAnalysis();
  LLVMCFLAliasAnalysis(LLVMCFLAliasAnalysis &&) noexcept;
  LLVMCFLAliasAnalysis &operator=(LLVMCFLAliasAnalysis &&) noexcept;
  LLVMCFLAliasAnalysis(const LLVMCFLAliasAnalysis &) = delete;
  LLVMCFLAliasAnalysis &operator=(const LLVMCFLAliasAnalysis &) = delete;

  ReachabilityStats analyze(llvm::Module &module);
  bool mayAlias(const llvm::Value *lhs, const llvm::Value *rhs) const;
  std::vector<const llvm::Value *> pointsTo(const llvm::Value *pointer) const;
  std::optional<std::size_t> nodeForValue(const llvm::Value *value) const;

  const AliasClient &client() const;
  const ReachabilityStats &statistics() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical
