/*
 * Taint Analysis Implementation - Reporting and Tracing
 *
 * Author: rainoftime
 */

#include "Annotation/Taint/TaintConfigManager.h"
#include "Dataflow/IFDS/Clients/IFDSTaintAnalysis.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/Support/raw_ostream.h>

namespace ifds {

// ============================================================================
// Summary Edge-Based Trace Reconstruction
// ============================================================================

// Internal template implementation to avoid code duplication
template <typename SolverType>
TaintAnalysis::TaintPath
trace_taint_sources_impl(const TaintAnalysis &self, const SolverType &solver,
                         const llvm::CallBase *sink_call,
                         const TaintFact &tainted_fact) {
  (void)tainted_fact; // Suppress unused parameter warning

  TaintAnalysis::TaintPath result;

  // Get all summary edges from the solver
  std::vector<SummaryEdge<TaintFact>> summary_edges;
  solver.get_summary_edges(summary_edges);

  // Build maps for efficient lookup
  std::unordered_map<const llvm::CallBase *,
                     std::vector<SummaryEdge<TaintFact>>>
      call_to_summaries;
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::CallBase *>>
      function_to_calls;

  for (const auto &edge : summary_edges) {
    call_to_summaries[edge.call_site].push_back(edge);

    // Build call graph
    const llvm::Function *caller_func = edge.call_site->getFunction();
    function_to_calls[caller_func].push_back(edge.call_site);
  }

  // Use a more sophisticated approach: trace back through the summary edges
  // starting from the sink call and following the taint flow backwards
  std::unordered_set<const llvm::CallBase *> visited_calls;
  std::unordered_set<const llvm::Function *> visited_functions;
  std::vector<const llvm::CallBase *> worklist;
  worklist.push_back(sink_call);

  while (!worklist.empty() && result.sources.size() < 10) {
    const llvm::CallBase *current_call = worklist.back();
    worklist.pop_back();

    if (visited_calls.count(current_call))
      continue;
    visited_calls.insert(current_call);

    const llvm::Function *current_func = current_call->getFunction();

    // Check if this is a source function
    if (self.is_source(current_call)) {
      result.sources.push_back(current_call);
      continue;
    }

    // Track intermediate functions
    if (visited_functions.insert(current_func).second &&
        result.intermediate_functions.size() < 10) {
      result.intermediate_functions.push_back(current_func);
    }

    // Check for initial taint from function entry
    if (current_call == &current_func->getEntryBlock().front()) {
      result.sources.push_back(current_call);
      continue;
    }

    // Find all summary edges for this call site
    auto summaries = call_to_summaries.find(current_call);
    if (summaries != call_to_summaries.end()) {
      for (const auto &summary : summaries->second) {
        (void)summary; // Suppress unused variable warning
        // Check if this summary edge could contribute to our tainted fact
        // This is where we'd need more sophisticated taint flow analysis
        // For now, we'll use a simplified approach

        // Look for calls in the same function that could flow taint to this
        // call
        auto calls_in_func = function_to_calls.find(current_func);
        if (calls_in_func != function_to_calls.end()) {
          for (const llvm::CallBase *other_call : calls_in_func->second) {
            if (other_call != current_call &&
                self.comes_before(other_call, current_call)) {
              // Check if this other call's summary edges could flow to our call
              auto other_summaries = call_to_summaries.find(other_call);
              if (other_summaries != call_to_summaries.end()) {
                for (const auto &other_summary : other_summaries->second) {
                  (void)other_summary; // Suppress unused variable warning
                  // Simplified check: if the return fact could flow to our call
                  // fact
                  if (!visited_calls.count(other_call)) {
                    worklist.push_back(other_call);
                  }
                }
              }
            }
          }
        }
      }
    }

    // Also look for inter-procedural flows (calls from other functions)
    // This requires more sophisticated call graph analysis
    for (const auto &edge : summary_edges) {
      if (edge.call_site->getCalledFunction() == current_func) {
        // This call invokes the current function - trace back to the caller
        if (!visited_calls.count(edge.call_site)) {
          worklist.push_back(edge.call_site);
        }
      }
    }
  }

  // Reverse intermediate functions so they go from source to sink
  std::reverse(result.intermediate_functions.begin(),
               result.intermediate_functions.end());

  return result;
}

// Public API methods that delegate to the template implementation
TaintAnalysis::TaintPath TaintAnalysis::trace_taint_sources_summary_based(
    const IFDSSolver<TaintAnalysis> &solver, const llvm::CallBase *sink_call,
    const TaintFact &tainted_fact) const {
  return trace_taint_sources_impl(*this, solver, sink_call, tainted_fact);
}

// Helper to check if one instruction comes before another in the same function
bool TaintAnalysis::comes_before(const llvm::Instruction *first,
                                 const llvm::Instruction *second) const {
  if (first->getFunction() != second->getFunction())
    return false;

  const llvm::BasicBlock *first_bb = first->getParent();
  const llvm::BasicBlock *second_bb = second->getParent();

  // Simple check: if they're in different basic blocks, check block order
  if (first_bb != second_bb) {
    for (const llvm::BasicBlock &bb : *first->getFunction()) {
      if (&bb == first_bb)
        return true;
      if (&bb == second_bb)
        return false;
    }
    return false;
  }

  // Same basic block - check instruction order
  for (const llvm::Instruction &inst : *first_bb) {
    if (&inst == first)
      return true;
    if (&inst == second)
      return false;
  }
  return false;
}

// Helper function to output vulnerability report
void TaintAnalysis::output_vulnerability_report(
    llvm::raw_ostream &OS, size_t vuln_num, const std::string &func_name,
    const llvm::CallBase *call, const std::string &tainted_args,
    const std::vector<const llvm::Instruction *> &all_sources,
    const std::vector<const llvm::Function *> &propagation_path,
    size_t max_vulnerabilities) const {
  if (vuln_num > max_vulnerabilities)
    return;

  OS << "\nVULNERABILITY #" << vuln_num << ":\n";
  OS << "  Sink: " << func_name << " (" << call->getDebugLoc() << ")\n";
  OS << "  Tainted args: " << tainted_args << "\n";

  // Display sources
  if (!all_sources.empty()) {
    OS << "  Sources:\n";
    std::unordered_set<const llvm::Instruction *> unique_sources(
        all_sources.begin(), all_sources.end());
    int source_num = 1;
    for (const auto *source : unique_sources) {
      if (auto *source_call = llvm::dyn_cast<llvm::CallInst>(source)) {
        if (source_call->getCalledFunction()) {
          std::string source_func = taint_config::normalize_name(
              source_call->getCalledFunction()->getName().str());
          OS << "    " << source_num++ << ". " << source_func << " ("
             << source->getFunction()->getName() << ":"
             << source_call->getDebugLoc() << ")\n";
        }
      } else {
        if (source == &source->getFunction()->getEntryBlock().front()) {
          OS << "    " << source_num++
             << ". [Entry: " << source->getFunction()->getName() << "]\n";
        } else {
          OS << "    " << source_num++
             << ". [Instr: " << source->getFunction()->getName() << ":"
             << source->getDebugLoc() << "]\n";
        }
      }
    }
  } else {
    OS << "  Sources: [Complex flow]\n";
  }

  // Display propagation path (intermediate functions)
  if (!propagation_path.empty() && propagation_path.size() > 1) {
    OS << "  Path: ";
    for (size_t i = 0; i < propagation_path.size() && i < 6; ++i) {
      if (i > 0)
        OS << " → ";
      OS << propagation_path[i]->getName().str();
    }
    if (propagation_path.size() > 6) {
      OS << " → ... (+" << (propagation_path.size() - 6) << ")";
    }
    OS << " → " << call->getFunction()->getName().str() << "\n";
  } else if (!all_sources.empty()) {
    OS << "  Path: Same function (" << call->getFunction()->getName().str()
       << ")\n";
  }
}

// Template function for vulnerability reporting with different solver types
// Simplified to just count reachable sinks
template <typename SolverType>
void report_vulnerabilities_impl(const TaintAnalysis &self,
                                 const SolverType &solver,
                                 llvm::raw_ostream &OS,
                                 size_t max_vulnerabilities) {
  (void)max_vulnerabilities; // Suppress unused parameter warning

  OS << "\nTaint Analysis Results:\n";
  OS << "========================\n";

  size_t reachable_sinks = 0;

  // Get all results from the solver
  auto results = solver.get_all_results();

  for (const auto &pair : results) {
    const typename SolverType::Node &node = pair.first;
    const typename SolverType::FactSet &facts = pair.second;

    if (facts.empty() || !node.instruction)
      continue;

    auto *call = llvm::dyn_cast<llvm::CallInst>(node.instruction);
    if (!call || !self.is_sink(call))
      continue;

    // Check if any argument is tainted
    bool found_tainted = false;
    for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
      const llvm::Value *arg = call->getOperand(i);
      for (const auto &fact : facts) {
        if (self.is_argument_tainted(arg, fact)) {
          reachable_sinks++;
          found_tainted = true;
          break; // Count this sink once and move to next
        }
      }
      if (found_tainted)
        break;
    }
  }

  if (reachable_sinks == 0) {
    OS << "No reachable sinks detected.\n";
  } else {
    OS << "Summary: " << reachable_sinks << " reachable sinks detected.\n";
  }
}

void TaintAnalysis::report_vulnerabilities(
    const IFDSSolver<TaintAnalysis> &solver, llvm::raw_ostream &OS,
    size_t max_vulnerabilities) const {
  report_vulnerabilities_impl(*this, solver, OS, max_vulnerabilities);
}

} // namespace ifds
