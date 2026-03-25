//===- SVFGEdge.h -- SVFG Edge Definitions
//------------------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//
//
// SVFGEdge: Edge Type Hierarchy for Sparse Value-Flow Graph
//
// This file defines the complete hierarchy of SVFG edge types, representing
// different kinds of value-flow relationships between SVFG nodes.
//
// == Edge Type Hierarchy ==
//
// SVFGEdge (base)
// ├── Intra-procedural edges (within a function)
// │   ├── IntraCopyVFGEdge     // x = y (direct copy)
// │   ├── IntraLoadVFGEdge     // x = *p (load from memory)
// │   ├── IntraStoreVFGEdge    // *p = x (store to memory)
// │   ├── IntraGepVFGEdge      // x = p + offset (pointer arithmetic)
// │   ├── IntraPhiVFGEdge      // phi node operand (merge point)
// │   └── IntraBinaryOpVFGEdge // x = a + b (arithmetic/logical)
// │
// ├── Call edges (caller → callee)
// │   ├── CallDirVFGEdge       // Direct call parameter passing
// │   └── CallIndVFGEdge       // Indirect call parameter passing
// │
// ├── Return edges (callee → caller)
// │   ├── RetDirVFGEdge        // Direct return value flow
// │   └── RetIndVFGEdge        // Indirect return value flow
// │
// └── Memory edges (Memory SSA)
//     ├── MUEdge               // Memory use (read) edge
//     ├── CHIEdge              // Memory def (write) edge
//     └── MHPEdge              // May-happen-in-parallel (threading)
//
// == Edge Semantics ==
//
// 1. Intra-procedural edges:
//    - Connect nodes within the same function
//    - Represent direct value-flow (def-use chains)
//    - Example: x = y creates IntraCopyVFGEdge from y's def to x's use
//
// 2. Call edges:
//    - Connect actual arguments at call site to formal parameters in callee
//    - Direct: Statically resolved function calls
//    - Indirect: Function pointer calls (resolved via pointer analysis)
//    - Example: foo(x) creates CallDirVFGEdge from x to foo's parameter
//
// 3. Return edges:
//    - Connect return values in callee to return uses at call site
//    - Direct: Statically resolved returns
//    - Indirect: Returns through function pointers
//    - Example: return x creates RetDirVFGEdge from x to call site
//
// 4. Memory edges (MU/CHI):
//    - MU edges: Connect memory definitions to loads (memory uses)
//      Example: *p = 5; x = *q; (if p and q may alias)
//               StoreChi(mem_2) --MU--> LoadMu(mem_2)
//    - CHI edges: Connect memory definitions to subsequent definitions
//      Example: *p = 5; *q = 10; (if p and q may alias)
//               StoreChi(mem_2) --CHI--> StoreChi(mem_3)
//
// == Edge Weights ==
//
// Edges carry weights for cost-sensitive analysis:
// - Zero: No cost (epsilon transitions, e.g., phi edges)
// - One: Standard weight (most edges)
// - Many: Unbounded cost (loop back-edges, recursion)
//
// Weights are used in shortest-path algorithms for demand-driven analysis.
//
// == Points-To Guards ==
//
// Memory edges carry points-to sets as guards:
// - Guard specifies which memory objects the edge applies to
// - Example: Load edge guarded by {obj_1, obj_2} means the load may read
//   from either object 1 or object 2
// - Enables precise memory dependence tracking
//
// == Call Site Association ==
//
// Call and return edges link to their call sites:
// - Enables context-sensitive analysis
// - Supports call graph traversal
// - Provides source location for debugging
//
// == Key Design Features ==
//
// - Type-safe casting: Use llvm::dyn_cast<EdgeType>(edge) for downcasting
// - Edge classification: isIntraEdge(), isCallEdge(), isRetEdge(), etc.
// - Weight management: getWeight(), setWeight() for cost-sensitive analysis
// - Points-to tracking: getPointsTo(), addPointsTo() for memory edges
//
//===----------------------------------------------------------------------===//

#pragma once

#include "IR/SVFG/SVFGBase.h"

#include <set>
#include <string>
#include <utility>

#include <llvm/IR/InstrTypes.h>

namespace lotus {
namespace analysis {

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(deprecated)
#define LOTUS_SVFG_LEGACY_API(MSG) [[deprecated(MSG)]]
#else
#define LOTUS_SVFG_LEGACY_API(MSG)
#endif
#else
#define LOTUS_SVFG_LEGACY_API(MSG)
#endif

class SVFGNode;

/// @brief Value-flow edge connecting SVFG nodes
class SVFGEdge {
public:
  /// @brief Edge weight for analysis cost estimation
  enum class EdgeWeight : uint8_t {
    Zero = 0, // No cost (e.g., epsilon transitions)
    One = 1,  // Standard weight
    Many      // Unbounded (for loops, recursion)
  };

private:
  SVFGNode *src;
  SVFGNode *dst;
  SVFGEdgeK kind;
  EdgeWeight weight;
  const llvm::CallBase *callSite;
  std::string callSiteDebug;
  std::set<uint32_t> pointsTo;

public:
  /// @brief Construct edge
  SVFGEdge(SVFGNode *s, SVFGNode *d, SVFGEdgeK k,
           EdgeWeight w = EdgeWeight::One, const llvm::CallBase *cs = nullptr,
           std::string csDebug = {}, std::set<uint32_t> pts = {})
      : src(s), dst(d), kind(k), weight(w), callSite(cs),
        callSiteDebug(std::move(csDebug)), pointsTo(std::move(pts)) {}

  /// @brief Destructor
  virtual ~SVFGEdge() = default;

  //===------------------------------------------------------------------===
  // Accessors
  //===------------------------------------------------------------------===

  inline SVFGNode *getSrcNode() const { return src; }
  inline SVFGNode *getDstNode() const { return dst; }
  inline SVFGEdgeK getEdgeKind() const { return kind; }
  inline EdgeWeight getWeight() const { return weight; }
  inline void setWeight(EdgeWeight w) { weight = w; }
  inline const llvm::CallBase *getCallSite() const { return callSite; }
  inline bool hasCallSite() const { return callSite != nullptr; }
  inline const std::string &getCallSiteDebug() const { return callSiteDebug; }
  inline void setCallSiteDebug(std::string s) { callSiteDebug = std::move(s); }
  inline bool hasCallSiteDebug() const { return !callSiteDebug.empty(); }
  inline const std::set<uint32_t> &getPointsTo() const { return pointsTo; }
  inline bool addPointsTo(const std::set<uint32_t> &pts) {
    const size_t oldSize = pointsTo.size();
    pointsTo.insert(pts.begin(), pts.end());
    return pointsTo.size() != oldSize;
  }

  //===------------------------------------------------------------------===
  // Classification
  //===------------------------------------------------------------------===

  inline bool isIntraEdge() const { return isIntraVFGEdge(kind); }
  inline bool isCallEdge() const { return isCallVFGEdge(kind); }
  inline bool isRetEdge() const { return isRetVFGEdge(kind); }
  inline bool isMemoryEdge() const { return isMemVFGEdge(kind); }
  inline bool isCopyEdge() const { return kind == SVFGEdgeK::IntraCopy; }
  inline bool isLoadEdge() const { return kind == SVFGEdgeK::IntraLoad; }
  inline bool isStoreEdge() const { return kind == SVFGEdgeK::IntraStore; }
  inline bool isPhiEdge() const { return kind == SVFGEdgeK::IntraPhi; }
  inline bool isIndirectEdge() const { return isIndirectVFGEdge(kind); }
  inline bool isThreadMHPEdge() const { return isThreadMHPVFGEdge(kind); }
  inline bool isDirectEdge() const { return isDirectVFGEdge(kind); }

  //===------------------------------------------------------------------===
  // Utility
  //===------------------------------------------------------------------===

  std::string toString() const;
};

/// @brief Intra-procedural copy edge
class IntraCopyVFGEdge : public SVFGEdge {
public:
  IntraCopyVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraCopy) {}
  SVFG_EDGE_KIND(IntraCopy)
};

/// @brief Intra-procedural load edge
class IntraLoadVFGEdge : public SVFGEdge {
public:
  IntraLoadVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraLoad) {}
  SVFG_EDGE_KIND(IntraLoad)
};

/// @brief Intra-procedural store edge
class IntraStoreVFGEdge : public SVFGEdge {
public:
  IntraStoreVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraStore) {}
  SVFG_EDGE_KIND(IntraStore)
};

/// @brief Intra-procedural GEP edge
class IntraGepVFGEdge : public SVFGEdge {
public:
  IntraGepVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraGep) {}
  SVFG_EDGE_KIND(IntraGep)
};

/// @brief Intra-procedural PHI edge
class IntraPhiVFGEdge : public SVFGEdge {
public:
  IntraPhiVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraPhi) {}
  SVFG_EDGE_KIND(IntraPhi)
};

/// @brief Memory use edge (load)
class LOTUS_SVFG_LEGACY_API(
    "Legacy wrapper; use SVFGEdge with SVFGEdgeK::IntraMu") IntraMuVFGEdge
    : public SVFGEdge {
public:
  IntraMuVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraMu) {}
  SVFG_EDGE_KIND(IntraMu)
};

/// @brief Memory def edge (store)
class LOTUS_SVFG_LEGACY_API(
    "Legacy wrapper; use SVFGEdge with SVFGEdgeK::IntraChi") IntraChiVFGEdge
    : public SVFGEdge {
public:
  IntraChiVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraChi) {}
  SVFG_EDGE_KIND(IntraChi)
};

/// @brief Direct call edge
class CallDirVFGEdge : public SVFGEdge {
public:
  CallDirVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::CallDir) {}
  SVFG_EDGE_KIND(CallDir)
};

/// @brief Indirect call edge
class CallIndVFGEdge : public SVFGEdge {
public:
  CallIndVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::CallInd) {}
  SVFG_EDGE_KIND(CallInd)
};

/// @brief Call actual-in to formal-in edge
class LOTUS_SVFG_LEGACY_API(
    "Legacy wrapper; use SVFGEdge with SVFGEdgeK::CallAIn") CallAInVFGEdge
    : public SVFGEdge {
public:
  CallAInVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::CallAIn) {}
  SVFG_EDGE_KIND(CallAIn)
};

/// @brief Call formal-in edge
class LOTUS_SVFG_LEGACY_API(
    "Legacy compatibility wrapper; canonical runtime edge kind is CallAIn")
    CallFInVFGEdge : public SVFGEdge {
public:
  CallFInVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::CallFIn) {}
  SVFG_EDGE_KIND(CallFIn)
};

/// @brief Parameter call edge
class LOTUS_SVFG_LEGACY_API("Legacy compatibility wrapper; canonical runtime "
                            "edge kind is CallDir or CallInd") ParamCallVFGEdge
    : public SVFGEdge {
public:
  ParamCallVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::ParamCall) {}
  SVFG_EDGE_KIND(ParamCall)
};

/// @brief Direct return edge
class RetDirVFGEdge : public SVFGEdge {
public:
  RetDirVFGEdge(SVFGNode *s, SVFGNode *d) : SVFGEdge(s, d, SVFGEdgeK::RetDir) {}
  SVFG_EDGE_KIND(RetDir)
};

/// @brief Indirect return edge
class RetIndVFGEdge : public SVFGEdge {
public:
  RetIndVFGEdge(SVFGNode *s, SVFGNode *d) : SVFGEdge(s, d, SVFGEdgeK::RetInd) {}
  SVFG_EDGE_KIND(RetInd)
};

/// @brief Return actual-out to formal-out edge
class LOTUS_SVFG_LEGACY_API(
    "Legacy wrapper; use SVFGEdge with SVFGEdgeK::RetAOut") RetAOutVFGEdge
    : public SVFGEdge {
public:
  RetAOutVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::RetAOut) {}
  SVFG_EDGE_KIND(RetAOut)
};

/// @brief Return formal-out edge
class LOTUS_SVFG_LEGACY_API(
    "Legacy compatibility wrapper; canonical runtime edge kind is RetAOut")
    RetFOutVFGEdge : public SVFGEdge {
public:
  RetFOutVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::RetFOut) {}
  SVFG_EDGE_KIND(RetFOut)
};

/// @brief Parameter return edge
class LOTUS_SVFG_LEGACY_API("Legacy compatibility wrapper; canonical runtime "
                            "edge kind is RetDir or RetInd") ParamRetVFGEdge
    : public SVFGEdge {
public:
  ParamRetVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::ParamRet) {}
  SVFG_EDGE_KIND(ParamRet)
};

/// @brief Intra-procedural indirect value-flow edge
/// Represents indirect value flows through pointers (load, store, etc.)
/// Carries points-to set for precision in alias analysis.
class LOTUS_SVFG_LEGACY_API(
    "Legacy wrapper; use SVFGEdge with SVFGEdgeK::IntraIndirect")
    IntraIndirectVFGEdge : public SVFGEdge {
public:
  IntraIndirectVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::IntraIndirect) {}
  SVFG_EDGE_KIND(IntraIndirect)
};

/// @brief Thread MHP (May-Happen-in-Parallel) indirect edge
/// Represents indirect value-flows between memory accesses that may happen
/// in parallel in multithreaded programs. Used for concurrent analysis.
class LOTUS_SVFG_LEGACY_API(
    "Legacy wrapper; use SVFGEdge with SVFGEdgeK::ThreadMHPIndirectVF")
    ThreadMHPIndVFGEdge : public SVFGEdge {
public:
  ThreadMHPIndVFGEdge(SVFGNode *s, SVFGNode *d)
      : SVFGEdge(s, d, SVFGEdgeK::ThreadMHPIndirectVF) {}
  SVFG_EDGE_KIND(ThreadMHPIndirectVF)
};

} // namespace analysis
} // namespace lotus

#undef LOTUS_SVFG_LEGACY_API
