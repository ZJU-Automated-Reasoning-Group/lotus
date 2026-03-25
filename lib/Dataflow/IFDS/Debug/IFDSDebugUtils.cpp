/*
 * IFDS Framework Debug Utilities
 *
 * This file contains debugging and utility functions for the IFDS framework.
 * Separated from the main implementation to improve code organization.
 *
 * Author: rainoftime
 */

#include "Dataflow/IFDS/Clients/IFDSReachingDefinitions.h"
#include "Dataflow/IFDS/Clients/IFDSTaintAnalysis.h"
#include "Dataflow/IFDS/Core/IFDSFramework.h"

#include <llvm/Support/raw_ostream.h>

namespace ifds {

// ============================================================================
// Debug Utilities
// ============================================================================

template <typename Fact>
void debug_print_path_edges(
    const std::unordered_set<PathEdge<Fact>, PathEdgeHash<Fact>> &edges) {
  llvm::errs() << "Path Edges (" << edges.size() << "):\n";
  for (const auto &edge : edges) {
    llvm::errs() << "  " << edge.start_node->getOpcodeName() << " -> "
                 << edge.target_node->getOpcodeName() << "\n";
  }
}

template <typename Fact>
void debug_print_summary_edges(
    const std::unordered_set<SummaryEdge<Fact>, SummaryEdgeHash<Fact>> &edges) {
  llvm::errs() << "Summary Edges (" << edges.size() << "):\n";
  for (const auto &edge : edges) {
    llvm::errs() << "  Call: " << edge.call_site->getOpcodeName() << "\n";
  }
}

// Explicit template instantiations for debug utilities
template void debug_print_path_edges<TaintFact>(
    const std::unordered_set<PathEdge<TaintFact>, PathEdgeHash<TaintFact>> &);
template void debug_print_path_edges<DefinitionFact>(
    const std::unordered_set<PathEdge<DefinitionFact>,
                             PathEdgeHash<DefinitionFact>> &);
template void debug_print_summary_edges<TaintFact>(
    const std::unordered_set<SummaryEdge<TaintFact>, SummaryEdgeHash<TaintFact>>
        &);
template void debug_print_summary_edges<DefinitionFact>(
    const std::unordered_set<SummaryEdge<DefinitionFact>,
                             SummaryEdgeHash<DefinitionFact>> &);

} // namespace ifds
