//===- DDAPass.h -- DDA driver (SVF-style) -------------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// DDAPass: Demand-Driven Analysis Driver
//
// This file provides the main driver for demand-driven pointer analysis,
// allowing selection of analysis mode (flow-sensitive, context-sensitive)
// and client type (all pointers, function pointers, alias queries).
//
// == Analysis Modes ==
//
// - FlowS_DDA (FlowDDA): Flow-sensitive, context-insensitive
//   - Distinguishes different program points
//   - Merges all calling contexts
//   - Faster, less precise for recursive/callback-heavy code
//
// - Cxt_DDA (ContextDDA): Flow-sensitive, context-sensitive
//   - Distinguishes different program points AND calling contexts
//   - More precise but slower
//   - Better for recursive functions and callbacks
//
// == Client Types ==
//
// - All: Analyze all top-level pointers in the program
//   - Use for whole-program analysis
//   - Comprehensive but may be slow for large programs
//
// - Funptr: Analyze only function pointers at indirect call sites
//   - Use for call graph construction
//   - Efficient for resolving virtual calls and callbacks
//
// - Alias: Analyze pointers in loads, stores, and GEPs
//   - Use for alias-driven optimizations
//   - Focuses on memory-related pointers
//
// == Usage Example ==
//
// ```cpp
// DDAPass dda;
// dda.setDDAKind(DDAKind::FlowS_DDA);
// dda.selectClient(DDAClientKind::Funptr);
// dda.runOnModule(module);
//
// // Query results
// if (dda.mayAlias(ptr1, ptr2)) {
//   // ptr1 and ptr2 may alias
// }
// ```
//
// == Custom Queries ==
//
// ```cpp
// DDAPass dda;
// dda.addQuery(ptr1);  // Add specific pointer to query
// dda.addQuery(ptr2);
// dda.runOnModule(module);
// ```
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Alias/DDA/DDAClient.h"
#include "Alias/DDA/FlowDDA.h"

#include <memory>

namespace llvm {
class Module;
class Value;
} // namespace llvm

namespace lotus {
namespace analysis {

class ContextDDA;

enum class DDAKind {
  FlowS_DDA, /// Flow-sensitive, context-insensitive (FlowDDA)
  Cxt_DDA    /// Flow-sensitive, context-sensitive (ContextDDA)
};

enum class DDAClientKind {
  All,    /// All top-level pointers (DDAClient, solveAll)
  Funptr, /// Function pointers at indirect call sites
  Alias   /// Load src, store dst, GEP src (AliasDDAClient)
};

/// Demand-driven analysis driver: mode + client, run + answerQueries.
class DDAPass {
public:
  DDAPass() = default;
  ~DDAPass();

  void runOnModule(llvm::Module &M);
  void selectClient(DDAClientKind k);
  void setClient(std::unique_ptr<DDAClient> client);
  DDAClient *getClient() const { return client_.get(); }
  /// Add one explicit query pointer (switches client to user-query mode).
  void addQuery(const llvm::Value *v);

  void setDDAKind(DDAKind k) { kind_ = k; }
  DDAKind getDDAKind() const { return kind_; }

  void setMaxContextLen(uint32_t max) { maxContextLen_ = max; }
  uint32_t getMaxContextLen() const { return maxContextLen_; }

  void setMaxPathLen(uint32_t max) { maxPathLen_ = max; }
  uint32_t getMaxPathLen() const { return maxPathLen_; }

  void setMaxBudget(uint32_t max) { maxBudget_ = max; }
  uint32_t getMaxBudget() const { return maxBudget_; }

  void setInsensitiveRecursion(bool enable) {
    insensitiveRecursion_ = enable;
  }
  bool getInsensitiveRecursion() const { return insensitiveRecursion_; }

  void setInsensitiveCycle(bool enable) { insensitiveCycle_ = enable; }
  bool getInsensitiveCycle() const { return insensitiveCycle_; }

  FlowDDA *getFlowDDA() const { return flowDDA_.get(); }
  ContextDDA *getContextDDA() const { return contextDDA_.get(); }
  /// Convenience alias query over the current FlowDDA results.
  bool mayAlias(const llvm::Value *v1, const llvm::Value *v2) const;

private:
  void runPointerAnalysis(llvm::Module &M, DDAKind k);

  DDAKind kind_ = DDAKind::FlowS_DDA;
  uint32_t maxContextLen_ = 3u;
  uint32_t maxPathLen_ = 0u;
  uint32_t maxBudget_ = 100000u;
  bool insensitiveRecursion_ = false;
  bool insensitiveCycle_ = false;
  std::unique_ptr<DDAClient> client_;
  std::unique_ptr<FlowDDA> flowDDA_;
  std::unique_ptr<ContextDDA> contextDDA_;
};

} // namespace analysis
} // namespace lotus
