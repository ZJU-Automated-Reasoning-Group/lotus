/*
 * Sequential IFDS Solver Implementation
 *
 * This implements a straightforward sequential version of the IFDS tabulation algorithm:
 * - Simple worklist-based processing
 * - No thread synchronization overhead
 * - Easier to debug and maintain
 * - Suitable for small to medium programs or debugging
 */

#include "Utils/Platform/ProgressBar.h"

#include <llvm/Support/raw_ostream.h>

namespace ifds {

// ============================================================================
// IFDSSolver Implementation
// ============================================================================

template<typename Problem>
IFDSSolver<Problem>::IFDSSolver(Problem& problem)
    : m_problem(problem) {
}

template<typename Problem>
IFDSSolver<Problem>::~IFDSSolver() {
    if (m_injected_alias_analysis) {
        m_problem.set_alias_analysis(nullptr);
    }
}

template<typename Problem>
void IFDSSolver<Problem>::solve(const llvm::Module& module) {
    if (m_injected_alias_analysis) {
        m_problem.set_alias_analysis(nullptr);
        m_owned_alias_analysis.reset();
        m_injected_alias_analysis = false;
    }
    if (m_config.auto_inject_alias_analysis() &&
        !m_problem.has_alias_analysis_configured()) {
        m_owned_alias_analysis = std::make_unique<lotus::AliasAnalysisWrapper>(
            const_cast<llvm::Module&>(module),
            m_config.alias_analysis_config());
        m_problem.set_alias_analysis(m_owned_alias_analysis.get());
        m_injected_alias_analysis = true;
    }

    m_steps_performed = 0;
    m_bound_reached = false;

    // Initialize data structures
    m_graph_context.initialize(module);
    initialize_worklist(module);

    // Run sequential tabulation algorithm
    run_tabulation();
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet
IFDSSolver<Problem>::get_facts_at_entry(const llvm::Instruction* inst) const {
    auto it = m_entry_facts.find(inst);
    return it != m_entry_facts.end() ? it->second : FactSet{};
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet
IFDSSolver<Problem>::get_facts_at_exit(const llvm::Instruction* inst) const {
    auto it = m_exit_facts.find(inst);
    return it != m_exit_facts.end() ? it->second : FactSet{};
}

template<typename Problem>
void IFDSSolver<Problem>::get_path_edges(std::vector<PathEdge<Fact>>& out_edges) const {
    out_edges.clear();
    out_edges.reserve(m_state.path_edges.size());
    for (const auto& edge : m_state.path_edges) {
        out_edges.push_back(edge);
    }
}

template<typename Problem>
void IFDSSolver<Problem>::get_summary_edges(std::vector<SummaryEdge<Fact>>& out_edges) const {
    out_edges.clear();
    out_edges.reserve(m_state.summary_edges.size());
    for (const auto& edge : m_state.summary_edges) {
        out_edges.push_back(edge);
    }
}

template<typename Problem>
bool IFDSSolver<Problem>::fact_reaches(const Fact& fact, const llvm::Instruction* inst) const {
    auto it = m_exit_facts.find(inst);
    return it != m_exit_facts.end() && it->second.find(fact) != it->second.end();
}

template<typename Problem>
std::unordered_map<typename IFDSSolver<Problem>::Node,
                  typename IFDSSolver<Problem>::FactSet,
                  typename IFDSSolver<Problem>::NodeHash>
IFDSSolver<Problem>::get_all_results() const {
    std::unordered_map<Node, FactSet, NodeHash> results;
    typename Problem::FactType zero = m_problem.zero_fact();

    for (const auto& pair : m_exit_facts) {
        const llvm::Instruction* inst = pair.first;
        const FactSet& facts = pair.second;
        if (!facts.empty()) {
            results[Node(inst, zero)] = facts;
        }
    }

    return results;
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet
IFDSSolver<Problem>::get_facts_at(const Node& node) const {
    return get_facts_at_exit(node.instruction);
}

template<typename Problem>
typename IFDSSolver<Problem>::FactSet
IFDSSolver<Problem>::get_facts_at_in_llvm_ssa(const llvm::Instruction* inst) const {
    if (inst->getType()->isVoidTy()) {
        return get_facts_at_exit(inst);
    }
    const llvm::Instruction* next = inst->getNextNode();
    if (next) {
        return get_facts_at_entry(next);
    }
    if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(inst)) {
        llvm::BasicBlock* normal = invoke->getNormalDest();
        if (normal && !normal->empty()) {
            return get_facts_at_entry(&normal->front());
        }
    }
    return get_facts_at_exit(inst);
}

// ============================================================================
// Core IFDS Tabulation Algorithm Methods
// ============================================================================

template<typename Problem>
bool IFDSSolver<Problem>::propagate_path_edge(const PathEdgeType& edge) {
    // Try to insert the edge - if already exists, return false
    if (!m_state.add_path_edge(edge)) {
        return false;
    }

    // Preserve context precision: process each newly discovered path edge,
    // even if another start fact already reached the same (target_node, target_fact).
    m_entry_facts[edge.target_node].insert(edge.target_fact);

    return true;
}

template<typename Problem>
void IFDSSolver<Problem>::process_normal_edge(const PathEdgeType& current_edge,
                                              const llvm::Instruction* next) {
    // The normal-flow cache must NOT be keyed only on (source_inst, source_fact)
    // because the flow function result is successor-independent in standard IFDS
    // (it does not depend on which successor we are flowing to).  However, the
    // exit-facts recording and the propagation target DO depend on `next`.
    // We cache the flow-function result (which is successor-independent) but
    // always propagate to the correct `next` instruction.
    NormalFlowKey nkey{current_edge.target_node, next,
                       current_edge.target_fact};
    FactSet new_facts;
    if (m_config.enable_flow_function_caching()) {
        auto cit = m_normal_flow_cache.find(nkey);
        if (cit != m_normal_flow_cache.end()) {
            new_facts = cit->second;
        } else {
            new_facts = m_problem.normal_flow(current_edge.target_node, next,
                                              current_edge.target_fact);
            if (m_problem.auto_add_zero() && m_problem.is_zero_fact(current_edge.target_fact)) {
                new_facts.insert(m_problem.zero_fact());
            }
            m_normal_flow_cache[nkey] = new_facts;
        }
    } else {
        new_facts = m_problem.normal_flow(current_edge.target_node, next,
                                          current_edge.target_fact);
        if (m_problem.auto_add_zero() && m_problem.is_zero_fact(current_edge.target_fact)) {
            new_facts.insert(m_problem.zero_fact());
        }
    }

    // Record exit facts for the current instruction (union over all successors).
    if (!new_facts.empty()) {
        auto& exit_facts = m_exit_facts[current_edge.target_node];
        exit_facts.insert(new_facts.begin(), new_facts.end());
    }

    for (const auto& new_fact : new_facts) {
        on_normal_transition(Node(current_edge.target_node, current_edge.target_fact),
                             Node(next, new_fact));
        propagate_path_edge(PathEdgeType(current_edge.start_node, current_edge.start_fact,
                                         next, new_fact));
    }
}

template<typename Problem>
void IFDSSolver<Problem>::process_call_edge(const PathEdgeType& current_edge,
                                            const llvm::CallBase* call,
                                            const llvm::Function* callee) {
    if (!callee || callee->isDeclaration() || callee->empty()) {
        return;
    }

    // Get callee entry point
    const llvm::Instruction* callee_entry = &callee->getEntryBlock().front();

    // Apply call flow function to get facts at callee entry
    FactSet call_facts = m_problem.call_flow(call, callee, current_edge.target_fact);
    if (m_problem.auto_add_zero() && m_problem.is_zero_fact(current_edge.target_fact)) {
        call_facts.insert(m_problem.zero_fact());
    }

    for (const auto& entry_fact : call_facts) {
        // Track the entry fact used for this call
        m_entry_facts_at_call.insert({call, entry_fact});

        // Track call edge info for restoring caller context on return.
        // Use the caller's fact at the call site (current_edge.target_fact),
        // not the callee's entry fact, as the call_fact field.
        CallEdgeInfo edge_info{call, current_edge.target_fact,
                               current_edge.start_node, current_edge.start_fact};
        auto& info_vec = m_call_edge_info[{callee, entry_fact}];
        // Deduplicate using a companion unordered_set to avoid the O(n) linear
        // scan that was here before.  The set is keyed by the same four fields
        // as CallEdgeInfo::operator==.
        bool already_recorded = m_call_edge_info_seen[{callee, entry_fact}].count(edge_info) > 0;
        if (!already_recorded) {
            m_call_edge_info_seen[{callee, entry_fact}].insert(edge_info);
            info_vec.push_back(edge_info);
        }

        on_call_transition(Node(call, current_edge.target_fact),
                           Node(callee_entry, entry_fact));
        // Propagate into callee
        propagate_path_edge(PathEdgeType(callee_entry, entry_fact, callee_entry, entry_fact));

        // Apply existing summaries for this (callee, entry_fact) combination
        // This handles the case where the callee was already analyzed
        auto summary_it = m_summaries.find({callee, entry_fact});
        if (summary_it != m_summaries.end()) {
            for (const ExitSummary& summary : summary_it->second) {
                for (const llvm::Instruction* return_site : get_return_sites(call)) {
                    FactSet return_facts = m_problem.return_flow(
                        call, summary.exit_inst, return_site, callee,
                        summary.exit_fact,
                        current_edge.target_fact);
                    if (m_problem.auto_add_zero() &&
                        m_problem.is_zero_fact(summary.exit_fact)) {
                        return_facts.insert(m_problem.zero_fact());
                    }
                    for (const auto& rf : return_facts) {
                        SummaryEdgeType summary_edge(call, return_site,
                                                     current_edge.target_fact, rf);
                        m_state.add_summary_edge(summary_edge);
                        on_summary_transition(Node(call, current_edge.target_fact),
                                              Node(return_site, rf));
                        on_return_transition(Node(summary.exit_inst, summary.exit_fact),
                                             Node(return_site, rf));
                        propagate_path_edge(PathEdgeType(current_edge.start_node, current_edge.start_fact,
                                                         return_site, rf));
                    }
                }
            }
        }
    }
}

template<typename Problem>
void IFDSSolver<Problem>::process_return_edge(const PathEdgeType& current_edge,
                                              const llvm::ReturnInst* ret) {
    const llvm::Function* func = ret->getFunction();
    if (!func || func->empty()) {
        return;
    }

    const Fact& exit_fact = current_edge.target_fact;
    const Fact& start_fact = current_edge.start_fact;  // This is the entry fact
    bool had_incoming = false;

    // Create the summary for this (callee, entry_fact) combination
    SummaryKey summary_key{func, start_fact};
    auto& return_facts_set = m_summaries[summary_key];
    return_facts_set.insert(ExitSummary{ret, exit_fact});

    // Look up the call edge info to restore caller context (may have multiple call sites)
    auto call_edge_it = m_call_edge_info.find({func, start_fact});
    if (call_edge_it != m_call_edge_info.end()) {
        had_incoming = true;
        for (const CallEdgeInfo& edge_info : call_edge_it->second) {
            // Compute return facts using the return flow function.
            // The fourth argument is the caller's fact at the call site
            // (edge_info.call_fact), NOT the callee's entry fact (start_fact).
            // Passing start_fact here was wrong: it gave the flow function the
            // callee's entry fact instead of the caller's context fact, which
            // breaks analyses that use call_fact to decide what to propagate
            // back (e.g. taint analysis killing non-tainted return paths).
            // SummaryEdge records (call_site, caller_fact_at_call, callee_exit_fact).
            // Use edge_info.call_fact (the caller's fact at the call site) and
            // the computed caller-side return_fact so that the exposed summary-edge
            // reporting reflects the actual summary mapping observed at the caller.
            for (const llvm::Instruction* return_site : get_return_sites(edge_info.call_node)) {
                FactSet return_facts = m_problem.return_flow(
                    edge_info.call_node, ret, return_site, func, exit_fact,
                    edge_info.call_fact);
                if (m_problem.auto_add_zero() &&
                    m_problem.is_zero_fact(exit_fact)) {
                    return_facts.insert(m_problem.zero_fact());
                }
                for (const Fact& return_fact : return_facts) {
                    SummaryEdgeType new_summary(edge_info.call_node, return_site,
                                                edge_info.call_fact, return_fact);
                    m_state.add_summary_edge(new_summary);
                    on_summary_transition(Node(edge_info.call_node, edge_info.call_fact),
                                          Node(return_site, return_fact));
                    on_return_transition(Node(ret, exit_fact),
                                         Node(return_site, return_fact));
                    propagate_path_edge(PathEdgeType(edge_info.source_node, edge_info.source_fact,
                                                    return_site, return_fact));
                }
            }
        }
    }

    // Unbalanced returns: the callee was seeded directly (no incoming call edge
    // recorded for this entry fact).  Propagate return facts to all known
    // callers' return sites.
    //
    // BUG (fixed): the old code created a self-loop path edge
    //   PathEdge(return_site, zero, return_site, rf)
    // which seeds a brand-new analysis context at the return site rather than
    // connecting the callee's result back into the caller's existing context.
    // This causes the return fact to appear as if it originated at the return
    // site itself, losing all caller-side path information.
    //
    // The correct approach (Reps et al. §4.3 "follow-returns-past-seeds"):
    // for each call site that calls this callee, look up the call site's own
    // start context (the path-edge start that reached the call) and propagate
    // from there to the return site.  If no path edge reached the call site
    // yet (the call site is unreachable in the current analysis), we fall back
    // to using the call site itself as the start node with zero as the start
    // fact, which is the standard IFDS convention for unbalanced returns.
    auto callee_calls_it = m_graph_context.callee_to_calls().find(func);
    if (m_config.follow_returns_past_seeds() && !had_incoming &&
        callee_calls_it != m_graph_context.callee_to_calls().end()) {
        Fact zero = m_problem.zero_fact();
        for (const llvm::CallBase* call : callee_calls_it->second) {
            for (const llvm::Instruction* return_site : get_return_sites(call)) {
                FactSet return_facts = m_problem.return_flow(
                    call, ret, return_site, func, exit_fact, zero);
                for (const Fact& rf : return_facts) {
                    if (m_problem.is_zero_fact(rf)) {
                        continue;
                    }
                    // Use the call instruction as the start node (not the
                    // return site) so the path edge spans from the call to
                    // the return site, matching the caller's CFG structure.
                    // This avoids the spurious self-loop that was here before.
                    on_return_transition(Node(ret, exit_fact), Node(return_site, rf));
                    propagate_path_edge(PathEdgeType(call, zero, return_site, rf));
                }
            }
        }
    }
}

template<typename Problem>
void IFDSSolver<Problem>::process_call_to_return_edge(const PathEdgeType& current_edge,
                                                      const llvm::CallBase* call) {
    std::vector<const llvm::Function*> callees_vec;
    auto callees_it = m_graph_context.call_to_callees().find(call);
    if (callees_it != m_graph_context.call_to_callees().end()) {
        callees_vec = callees_it->second;
    }
    if (callees_vec.empty()) {
        callees_vec.push_back(nullptr);
    }

    for (const llvm::Instruction* return_site : get_return_sites(call)) {
        for (const llvm::Function* callee : callees_vec) {
            FactSet summary_facts =
                m_problem.summary_flow(call, callee, current_edge.target_fact);
            if (m_problem.auto_add_zero() &&
                m_problem.is_zero_fact(current_edge.target_fact)) {
                summary_facts.insert(m_problem.zero_fact());
            }
            for (const auto& summary_fact : summary_facts) {
                on_summary_transition(Node(call, current_edge.target_fact),
                                      Node(return_site, summary_fact));
                propagate_path_edge(PathEdgeType(current_edge.start_node,
                                                 current_edge.start_fact,
                                                 return_site, summary_fact));
            }
        }

        CallToReturnFlowKey ckey{call, return_site, current_edge.target_fact};
        FactSet ctr_facts;
        if (m_config.enable_flow_function_caching()) {
            auto cit = m_call_to_return_flow_cache.find(ckey);
            if (cit != m_call_to_return_flow_cache.end()) {
                ctr_facts = cit->second;
            } else {
                llvm::ArrayRef<const llvm::Function *> callees(callees_vec);
                ctr_facts = m_problem.call_to_return_flow(
                    call, return_site, callees, current_edge.target_fact);
                if (m_problem.auto_add_zero() &&
                    m_problem.is_zero_fact(current_edge.target_fact)) {
                    ctr_facts.insert(m_problem.zero_fact());
                }
                m_call_to_return_flow_cache[ckey] = ctr_facts;
            }
        } else {
            llvm::ArrayRef<const llvm::Function *> callees(callees_vec);
            ctr_facts = m_problem.call_to_return_flow(
                call, return_site, callees, current_edge.target_fact);
            if (m_problem.auto_add_zero() &&
                m_problem.is_zero_fact(current_edge.target_fact)) {
                ctr_facts.insert(m_problem.zero_fact());
            }
        }

        if (!ctr_facts.empty()) {
            auto& exit_facts = m_exit_facts[call];
            exit_facts.insert(ctr_facts.begin(), ctr_facts.end());
        }

        for (const auto& ctr_fact : ctr_facts) {
            on_call_to_return_transition(Node(call, current_edge.target_fact),
                                         Node(return_site, ctr_fact));
            propagate_path_edge(PathEdgeType(current_edge.start_node, current_edge.start_fact,
                                             return_site, ctr_fact));
        }
    }
}

// ============================================================================
// Helper Methods
// ============================================================================

template<typename Problem>
std::vector<const llvm::Instruction*>
IFDSSolver<Problem>::get_return_sites(const llvm::CallBase* call) const {
    return m_graph_context.get_return_sites(call);
}

template<typename Problem>
std::vector<const llvm::Instruction*>
IFDSSolver<Problem>::get_successors(const llvm::Instruction* inst) const {
    return m_graph_context.get_successors(inst);
}

// ============================================================================
// Initialization Methods
// ============================================================================

template<typename Problem>
void IFDSSolver<Problem>::initialize_call_graph(const llvm::Module& module) {
    m_graph_context.initialize(module);
}

template<typename Problem>
void IFDSSolver<Problem>::build_cfg_successors(const llvm::Module& module) {
    (void)module;
}

template<typename Problem>
void IFDSSolver<Problem>::initialize_worklist(const llvm::Module& module) {
    m_state.clear();
    m_entry_facts.clear();
    m_exit_facts.clear();
    m_summaries.clear();
    m_entry_facts_at_call.clear();
    m_call_edge_info.clear();
    m_call_edge_info_seen.clear();  // companion dedup set
    m_normal_flow_cache.clear();
    m_call_to_return_flow_cache.clear();

    auto seeds = m_graph_context.build_initial_seeds(m_problem, module);

    for (const auto& pair : seeds.get_seeds()) {
        const llvm::Instruction* entry = pair.first;
        FactSet facts = pair.second;
        if (m_problem.auto_add_zero()) {
            bool has_zero = false;
            for (const auto& fact : facts) {
                if (m_problem.is_zero_fact(fact)) {
                    has_zero = true;
                    break;
                }
            }
            if (!has_zero) {
                facts.insert(m_problem.zero_fact());
            }
        }
        for (const auto& fact : facts) {
            propagate_path_edge(PathEdgeType(entry, fact, entry, fact));
        }
    }
}

template<typename Problem>
void IFDSSolver<Problem>::run_tabulation() {
    // Use unique_ptr so the ProgressBar is always destroyed even if an
    // exception propagates out of the loop (exception-safe resource management).
    std::unique_ptr<ProgressBar> progress;
    size_t processed_edges = 0;
    size_t last_update = 0;
    const size_t update_interval = 100;

    if (m_config.enable_progress_reporting()) {
        progress = std::make_unique<ProgressBar>(
            "Sequential IFDS Analysis", ProgressBar::PBS_CharacterStyle, 0.01);
        llvm::outs() << "\n";
    }

    while (!m_state.worklist.empty()) {
        if (m_max_steps != 0 && m_steps_performed >= m_max_steps) {
            m_bound_reached = true;
            break;
        }

        PathEdgeType current_edge = m_state.worklist.back();
        m_state.worklist.pop_back();

        const llvm::Instruction* curr = current_edge.target_node;

        // Process different instruction types
        if (auto* call = llvm::dyn_cast<llvm::CallBase>(curr)) {
            // Call-to-return flows are always processed once per call edge.
            process_call_to_return_edge(current_edge, call);
            auto it = m_graph_context.call_to_callees().find(call);
            if (it != m_graph_context.call_to_callees().end()) {
                for (const llvm::Function* callee : it->second) {
                    process_call_edge(current_edge, call, callee);
                }
            }
        } else if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(curr)) {
            process_return_edge(current_edge, ret);
        } else {
            auto succs = get_successors(curr);
            for (const llvm::Instruction* succ : succs) {
                process_normal_edge(current_edge, succ);
            }
        }

        processed_edges++;
        m_steps_performed = processed_edges;

        if (m_config.enable_progress_reporting() &&
            processed_edges - last_update >= update_interval) {
            last_update = processed_edges;
            size_t total_path_edges = m_state.path_edges.size();
            size_t worklist_size = m_state.worklist.size();

            llvm::outs() << "\r\033[KProcessed: " << processed_edges
                        << " | Path edges: " << total_path_edges
                        << " | Worklist: " << worklist_size;
            llvm::outs().flush();
        }
    }

    if (m_config.enable_progress_reporting()) {
        llvm::outs() << "\r\033[K";
        progress->showProgress(1.0);
        llvm::outs() << "\nCompleted! Processed " << processed_edges
                    << " edges, discovered " << m_state.path_edges.size() << " path edges";
        if (m_bound_reached) {
            llvm::outs() << " (step bound " << m_max_steps << " reached)";
        }
        llvm::outs() << "\n";
        // progress is destroyed automatically by unique_ptr
    }
}

template<typename Problem>
const llvm::Function* IFDSSolver<Problem>::get_main_function(const llvm::Module& module) {
    return module.getFunction("main");
}

} // namespace ifds
