//===- DDAClient.h -- DDA clients (SVF-style) -----------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// DDAClient: Query Selection for Demand-Driven Analysis
//
// This file defines client classes that select which pointers to analyze
// in demand-driven pointer analysis. Different clients focus on different
// subsets of pointers based on analysis goals.
//
// == Client Types ==
//
// 1. DDAClient (base): All top-level pointers or user-specified queries
//    - Use for: Comprehensive analysis, custom query sets
//    - Collects: All pointer-typed values in the program
//    - Example: Whole-program alias analysis
//
// 2. FunptrDDAClient: Function pointers at indirect call sites
//    - Use for: Call graph construction, virtual call resolution
//    - Collects: Function pointers used in indirect calls
//    - Example: Resolving callbacks, virtual methods
//
// 3. AliasDDAClient: Pointers in memory operations
//    - Use for: Alias-driven optimizations, memory analysis
//    - Collects: Load sources, store destinations, GEP bases
//    - Example: Redundant load elimination, store forwarding
//
// == Usage Example ==
//
// ```cpp
// // Use function pointer client
// FunptrDDAClient client;
// FlowDDA dda;
// dda.setClient(&client);
// dda.run(module);
// dda.answerQueries();  // Analyzes only function pointers
// ```
//
// == Custom Queries ==
//
// ```cpp
// DDAClient client;
// client.addQuery(ptr1);  // Add specific pointer
// client.addQuery(ptr2);
// FlowDDA dda;
// dda.setClient(&client);
// dda.run(module);
// dda.answerQueries();  // Analyzes only ptr1 and ptr2
// ```
//
// == Client Callbacks ==
//
// Clients can override methods to hook into DDA:
// - collectCandidateQueries(): Select which pointers to analyze
// - handleStatement(): Called during backward traversal
// - performStat(): Collect statistics after analysis
//
//===----------------------------------------------------------------------===//

#pragma once

#include "IR/SVFG/SVFGBase.h"

#include <unordered_set>
#include <vector>

namespace llvm {
class Value;
class CallBase;
class Module;
} // namespace llvm

namespace lotus {
namespace analysis {

class SVFG;
class SVFGNode;
class FlowDDA;
class ContextDDA;

/// Base DDA client: collects candidate pointers for demand-driven queries.
class DDAClient {
public:
  DDAClient() : svfg_(nullptr), solveAll_(true) {}
  virtual ~DDAClient() = default;

  void setSVFG(SVFG *g) { svfg_ = g; }
  SVFG *getSVFG() const { return svfg_; }
  void setModule(const llvm::Module *M) { module_ = M; }
  const llvm::Module *getModule() const { return module_; }

  /// Collect candidate pointers (Value*) to be queried. Uses getSVFG() (and
  /// getModule() for Funptr) if set.
  virtual std::vector<const llvm::Value *> &collectCandidateQueries();
  const std::vector<const llvm::Value *> &getCandidateQueries() const {
    return candidateQueries_;
  }

  /// Run DDA for each candidate (calls dda->getPointsTo for each).
  virtual void answerQueries(FlowDDA *dda);
  /// Run context-sensitive DDA for each candidate.
  virtual void answerQueries(ContextDDA *dda);

  /// Callback during backward traversal (optional).
  virtual void handleStatement(const SVFGNode *node, uint32_t curNodeId) {
    (void)node;
    (void)curNodeId;
  }

  /// Statistics after answerQueries (optional).
  virtual void performStat(FlowDDA *dda) { (void)dda; }
  /// Statistics after context-sensitive answerQueries (optional).
  virtual void performStat(ContextDDA *dda);

  void setSolveAll(bool v) { solveAll_ = v; }
  bool getSolveAll() const { return solveAll_; }
  void addQuery(const llvm::Value *v) {
    userQueries_.push_back(v);
    solveAll_ = false;
  }

protected:
  void addCandidate(const llvm::Value *v);
  void resetCandidateQueries();

  SVFG *svfg_ = nullptr;
  const llvm::Module *module_ = nullptr;
  std::vector<const llvm::Value *> candidateQueries_;
  std::vector<const llvm::Value *> userQueries_;
  std::unordered_set<const llvm::Value *> candidateQuerySet_;
  bool solveAll_;
};

/// Client that collects only function pointers at indirect call sites.
class FunptrDDAClient : public DDAClient {
public:
  FunptrDDAClient() = default;
  std::vector<const llvm::Value *> &collectCandidateQueries() override;
  void performStat(FlowDDA *dda) override;
};

/// Client that collects load pointer operands, store pointer operands, GEP base
/// pointers.
class AliasDDAClient : public DDAClient {
public:
  AliasDDAClient() = default;
  std::vector<const llvm::Value *> &collectCandidateQueries() override;
  void performStat(FlowDDA *dda) override;
};

} // namespace analysis
} // namespace lotus
