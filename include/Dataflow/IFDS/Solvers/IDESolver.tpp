/*
 * IDE Solver Implementation
 *
 * This implements the IDE (Interprocedural Distributive Environment) algorithm,
 * an extension of IFDS that propagates values along with dataflow facts.
 *
 * Key features:
 * - Summary edge reuse: Callees are analyzed once per calling context
 * - Edge function composition memoization: Avoids redundant function compositions
 */

#include "Dataflow/ControlFlow/InterCFG.h"
#include <llvm/IR/CFG.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <chrono>

namespace ifds {

// ============================================================================
// IDESolver Implementation
// ============================================================================

template<typename Problem>
IDESolver<Problem>::IDESolver(Problem& problem) : m_problem(problem) {}

template<typename Problem>
IDESolver<Problem>::~IDESolver() {
    if (m_injected_alias_analysis) {
        m_problem.set_alias_analysis(nullptr);
    }
}

// ============================================================================
// Helper Methods
// ============================================================================

template<typename Problem>
typename IDESolver<Problem>::EdgeFunctionPtr
IDESolver<Problem>::make_edge_function(const EdgeFunction& ef) {
    EdgeFunctionPtr result = std::make_shared<EdgeFunction>(ef);
    m_join_members[result.get()].insert(result.get());
    return result;
}

template<typename Problem>
typename IDESolver<Problem>::EdgeFunctionPtr
IDESolver<Problem>::compose_cached(EdgeFunctionPtr f1, EdgeFunctionPtr f2) {
    if (!m_config.enable_edge_function_caching()) {
        return make_edge_function(m_problem.compose(*f1, *f2));
    }
    // Check cache first
    ComposePair key{f1, f2};
    auto it = m_compose_cache.find(key);
    if (it != m_compose_cache.end()) {
        return it->second;
    }

    // Compose and cache
    EdgeFunction composed = m_problem.compose(*f1, *f2);
    EdgeFunctionPtr result = make_edge_function(composed);
    m_compose_cache[key] = result;
    return result;
}

template<typename Problem>
typename IDESolver<Problem>::EdgeFunctionPtr
IDESolver<Problem>::join_cached(EdgeFunctionPtr f1, EdgeFunctionPtr f2) {
    if (f1 == f2) {
        return f1;
    }
    if (join_contains(f1, f2)) {
        return f1;
    }
    if (join_contains(f2, f1)) {
        return f2;
    }
    if (!m_config.enable_edge_function_caching()) {
        EdgeFunction joined = m_problem.join_edge_functions(*f1, *f2);
        if (m_problem.edge_function_equivalent(joined, *f1)) {
            return f1;
        }
        if (m_problem.edge_function_equivalent(joined, *f2)) {
            return f2;
        }
        EdgeFunctionPtr result = make_edge_function(joined);
        record_join_members(result, f1, f2);
        return result;
    }
    ComposePair key{f1, f2};
    auto it = m_join_cache.find(key);
    if (it != m_join_cache.end()) {
        return it->second;
    }

    EdgeFunction joined = m_problem.join_edge_functions(*f1, *f2);
    if (m_problem.edge_function_equivalent(joined, *f1)) {
        m_join_cache[key] = f1;
        return f1;
    }
    if (m_problem.edge_function_equivalent(joined, *f2)) {
        m_join_cache[key] = f2;
        return f2;
    }
    EdgeFunctionPtr result = make_edge_function(joined);
    record_join_members(result, f1, f2);
    m_join_cache[key] = result;
    return result;
}

template<typename Problem>
bool IDESolver<Problem>::join_contains(EdgeFunctionPtr aggregate,
                                       EdgeFunctionPtr member) const {
    if (!aggregate || !member) {
        return false;
    }
    auto it = m_join_members.find(aggregate.get());
    if (it == m_join_members.end()) {
        return aggregate == member;
    }
    return it->second.count(member.get()) > 0;
}

template<typename Problem>
void IDESolver<Problem>::record_join_members(EdgeFunctionPtr aggregate,
                                             EdgeFunctionPtr f1,
                                             EdgeFunctionPtr f2) {
    auto &members = m_join_members[aggregate.get()];
    auto add_members = [&](EdgeFunctionPtr fn) {
        if (!fn) {
            return;
        }
        auto it = m_join_members.find(fn.get());
        if (it == m_join_members.end()) {
            members.insert(fn.get());
            return;
        }
        members.insert(it->second.begin(), it->second.end());
    };
    add_members(f1);
    add_members(f2);
    members.insert(aggregate.get());
}

template<typename Problem>
void IDESolver<Problem>::solve(const llvm::Module& module) {
    using Fact = typename Problem::FactType;
    using Value = typename Problem::ValueType;

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

    // Clear previous results and caches
    m_values.clear();
    m_jump_functions.clear();
    m_incoming.clear();
    m_end_summaries.clear();
    m_path_edges.clear();
    m_summary_edges.clear();
    m_compose_cache.clear();
    m_join_cache.clear();
    m_join_members.clear();
    m_normal_edge_cache.clear();
    m_call_to_return_edge_cache.clear();
    m_worklist.clear();
    m_statistics.reset();
    m_statistics.start_time = std::chrono::steady_clock::now();
    m_statistics.functions_analyzed = module.size();
    m_graph_context.initialize(module);

    auto get_return_sites = [&](const llvm::CallBase* call)
        -> std::vector<const llvm::Instruction*> {
        return m_graph_context.get_return_sites(call);
    };

    auto preserve_zero = [&](FactSet& facts, const Fact& source_fact) {
        if (m_problem.auto_add_zero() && m_problem.is_zero_fact(source_fact)) {
            facts.insert(m_problem.zero_fact());
        }
    };

    EdgeFunctionPtr identity_func = make_edge_function(m_problem.identity());

    auto add_jump_function = [&](const PathEdgeType& edge, EdgeFunctionPtr phi) {
        auto it = m_jump_functions.find(edge);
        if (it == m_jump_functions.end()) {
            m_jump_functions.emplace(edge, phi);
            m_worklist.emplace_back(edge, phi);
            if (m_config.record_edges() && m_path_edges.insert(edge).second) {
                on_path_edge_added(edge);
            }
            return;
        }

        EdgeFunctionPtr joined = join_cached(it->second, phi);
        // Use pointer identity to detect change: join_cached returns the
        // existing pointer unchanged when the join is idempotent (i.e. the
        // new phi is already subsumed).  If the pointer changed, the jump
        // function was updated and we must re-propagate.
        // The old semantic-equivalence probe (checking only top/bottom/join)
        // was unsound for multi-valued domains (e.g. integer constants, type
        // states) where two distinct functions can agree on those three probe
        // points yet differ on other inputs.
        if (it->second != joined) {
            it->second = joined;
            m_worklist.emplace_back(edge, joined);
        }
    };

    // add_incoming: record a call edge for a callee start key.
    // We do NOT store caller_phi in the incoming record because the jump
    // function for the caller path edge may be updated (joined) after the
    // incoming edge is first recorded.  Instead we store only the path-edge
    // identity (start_node, start_fact) and look up the current jump function
    // from m_jump_functions at summary-application time.
    auto add_incoming = [&](const StartKey& key, const IncomingEdge& incoming) {
        auto& list = m_incoming[key];
        // Deduplicate by structural identity (ignoring caller_phi which is
        // looked up dynamically).
        for (const auto& existing : list) {
            if (existing.call == incoming.call &&
                existing.call_fact == incoming.call_fact &&
                existing.start_node == incoming.start_node &&
                existing.start_fact == incoming.start_fact) {
                return;
            }
        }
        list.push_back(incoming);
    };

    auto add_summary = [&](const StartKey& key, const llvm::Instruction* exit_inst,
                           const Fact& exit_fact,
                           EdgeFunctionPtr phi) {
        auto& vec = m_end_summaries[key];
        for (const auto& summary : vec) {
            if (summary.exit_inst == exit_inst && summary.exit_fact == exit_fact &&
                summary.phi == phi) {
                return false;
            }
        }
        vec.push_back(EndSummary{exit_inst, exit_fact, phi});
        return true;
    };

    auto apply_summary_to_incoming = [&](const IncomingEdge& incoming,
                                         const llvm::Function* callee,
                                         const Fact& callee_fact,
                                         const llvm::Instruction* exit_inst,
                                         const Fact& exit_fact,
                                         EdgeFunctionPtr summary_phi) {
        auto call_ef = m_problem.call_edge_function(
            incoming.call, callee, incoming.call_fact, callee_fact);
        EdgeFunctionPtr call_phi = make_edge_function(call_ef);

        // Look up the *current* (possibly updated) jump function for the
        // caller path edge (start_node, start_fact) -> (call, call_fact).
        // Using the stale incoming.caller_phi would produce wrong composed
        // edge functions whenever the jump function was updated after the
        // incoming edge was first recorded.
        PathEdgeType caller_edge(incoming.start_node, incoming.start_fact,
                                 incoming.call, incoming.call_fact);
        auto jf_it = m_jump_functions.find(caller_edge);
        EdgeFunctionPtr current_caller_phi = (jf_it != m_jump_functions.end())
                                             ? jf_it->second
                                             : identity_func;

        for (const llvm::Instruction* ret_site : get_return_sites(incoming.call)) {
            FactSet return_facts = m_problem.return_flow(
                incoming.call, exit_inst, ret_site, callee, exit_fact,
                incoming.call_fact);
            preserve_zero(return_facts, exit_fact);
            for (const auto& ret_fact : return_facts) {
                SummaryEdge<Fact> summary_edge(incoming.call, ret_site,
                                               incoming.call_fact, ret_fact);
                if (m_config.record_edges() &&
                    m_summary_edges.insert(summary_edge).second) {
                    on_summary_edge_added(summary_edge);
                }
                auto ret_ef = m_problem.return_edge_function(
                    incoming.call, callee, exit_inst, ret_site, exit_fact,
                    ret_fact);
                EdgeFunctionPtr ret_phi = make_edge_function(ret_ef);
                EdgeFunctionPtr composed = compose_cached(ret_phi,
                                          compose_cached(summary_phi,
                                          compose_cached(call_phi, current_caller_phi)));
                on_summary_transition(Node(incoming.call, incoming.call_fact),
                                      Node(ret_site, ret_fact));
                on_return_transition(Node(exit_inst, exit_fact),
                                     Node(ret_site, ret_fact));
                add_jump_function(PathEdgeType(incoming.start_node, incoming.start_fact,
                                               ret_site, ret_fact),
                                  composed);
            }
        }
    };

    // Initialize initial seeds (Phasar-style: instruction -> fact -> value)
    auto ide_seeds = m_problem.initial_ide_seeds(module);

    for (const auto& pair : ide_seeds.get_seeds()) {
        const llvm::Instruction* entry = pair.first;
        FactSet facts;
        for (const auto& fv : pair.second) {
            facts.insert(fv.first);
        }
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
            add_jump_function(PathEdgeType(entry, fact, entry, fact), identity_func);
        }
    }

    // Phase 1: compute jump functions
    while (!m_worklist.empty()) {
        if (m_max_steps != 0 && m_steps_performed >= m_max_steps) {
            m_bound_reached = true;
            break;
        }

        auto work_item = m_worklist.back();
        m_worklist.pop_back();

        const PathEdgeType& edge = work_item.first;
        EdgeFunctionPtr phi = work_item.second;
        const llvm::Instruction* curr = edge.target_node;
        const Fact& start_fact = edge.start_fact;
        const Fact& fact = edge.target_fact;

        if (auto* call = llvm::dyn_cast<llvm::CallBase>(curr)) {
            auto it_callees = m_graph_context.call_to_callees().find(call);
            std::vector<const llvm::Function*> callees;
            if (it_callees != m_graph_context.call_to_callees().end()) {
                callees = it_callees->second;
            }
            if (callees.empty()) {
                callees.push_back(nullptr);
            }

            // Apply optional summary flow/edge functions (special-cased callees)
            for (const llvm::Function* callee : callees) {
                for (const llvm::Instruction* ret_site : get_return_sites(call)) {
                    FactSet summary_facts = m_problem.summary_flow(call, callee, fact);
                    preserve_zero(summary_facts, fact);
                    for (const auto& tgt_fact : summary_facts) {
                        auto ef = m_problem.summary_edge_function(
                            call, callee, ret_site, fact, tgt_fact);
                        EdgeFunctionPtr edge_fn = make_edge_function(ef);
                        EdgeFunctionPtr new_phi = compose_cached(edge_fn, phi);
                        on_summary_transition(Node(call, fact), Node(ret_site, tgt_fact));
                        add_jump_function(PathEdgeType(edge.start_node, start_fact,
                                                       ret_site, tgt_fact),
                                          new_phi);
                    }
                }
            }

            // Always generate call-to-return edges
            for (const llvm::Instruction* ret_site : get_return_sites(call)) {
                llvm::ArrayRef<const llvm::Function *> callee_set(callees);
                FactSet ctr_facts =
                    m_problem.call_to_return_flow(call, ret_site, callee_set,
                                                  fact);
                preserve_zero(ctr_facts, fact);
                for (const auto& tgt_fact : ctr_facts) {
                    EdgeFunctionPtr edge_fn;
                    if (m_config.enable_edge_function_caching()) {
                        CallToReturnEdgeKey ckey(call, ret_site, fact, tgt_fact);
                        auto eit = m_call_to_return_edge_cache.find(ckey);
                        if (eit != m_call_to_return_edge_cache.end()) {
                            edge_fn = eit->second;
                        } else {
                            edge_fn = make_edge_function(
                                m_problem.call_to_return_edge_function(
                                    call, ret_site, callee_set, fact,
                                    tgt_fact));
                            m_call_to_return_edge_cache[ckey] = edge_fn;
                        }
                    } else {
                        edge_fn = make_edge_function(
                            m_problem.call_to_return_edge_function(
                                call, ret_site, callee_set, fact, tgt_fact));
                    }
                    EdgeFunctionPtr new_phi = compose_cached(edge_fn, phi);
                    on_call_to_return_transition(Node(call, fact),
                                                 Node(ret_site, tgt_fact));
                    add_jump_function(PathEdgeType(edge.start_node, start_fact,
                                                   ret_site, tgt_fact),
                                      new_phi);
                }
            }

            for (const llvm::Function* callee : callees) {
                if (!callee || callee->isDeclaration() || callee->empty()) {
                    continue;
                }
                const llvm::Instruction* callee_entry = &callee->getEntryBlock().front();
                FactSet call_facts = m_problem.call_flow(call, callee, fact);
                preserve_zero(call_facts, fact);
                for (const auto& callee_fact : call_facts) {
                    StartKey key{callee_entry, callee_fact};
                    IncomingEdge incoming{call, fact, edge.start_node, start_fact};
                    add_incoming(key, incoming);
                    on_call_transition(Node(call, fact),
                                       Node(callee_entry, callee_fact));

                    // Seed callee with identity jump function
                    add_jump_function(PathEdgeType(callee_entry, callee_fact,
                                                   callee_entry, callee_fact),
                                      identity_func);

                    // Apply existing summaries for this callee start
                    auto summary_it = m_end_summaries.find(key);
                    if (summary_it != m_end_summaries.end()) {
                        for (const auto& summary : summary_it->second) {
                            apply_summary_to_incoming(incoming, callee, callee_fact,
                                                      summary.exit_inst,
                                                      summary.exit_fact,
                                                      summary.phi);
                        }
                    }
                }
            }

        } else if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(curr)) {
            const llvm::Function* func = ret->getFunction();
            if (!func || func->empty()) {
                continue;
            }
            const llvm::Instruction* entry = &func->getEntryBlock().front();
            StartKey key{entry, start_fact};

            if (add_summary(key, ret, fact, phi)) {
                auto incoming_it = m_incoming.find(key);
                if (incoming_it != m_incoming.end()) {
                    for (const auto& incoming : incoming_it->second) {
                        apply_summary_to_incoming(incoming, func, start_fact, ret,
                                                  fact, phi);
                    }
                }
            }

            // Unbalanced returns: no incoming call edge for (entry, start_fact).
            // This handles the case where a function is analyzed as a seed
            // (i.e. its entry is a seed node) but is also called from elsewhere
            // in the program.  We propagate the return value to those call sites
            // using the zero fact as the caller context.
            if (m_config.follow_returns_past_seeds()) {
                auto incoming_it = m_incoming.find(key);
                if (incoming_it == m_incoming.end() || incoming_it->second.empty()) {
                    auto callee_calls_it = m_graph_context.callee_to_calls().find(func);
                    if (callee_calls_it != m_graph_context.callee_to_calls().end()) {
                        Fact zero_fact = m_problem.zero_fact();
                        for (const llvm::CallBase* call : callee_calls_it->second) {
                            for (const llvm::Instruction* ret_site : get_return_sites(call)) {
                                FactSet return_facts =
                                    m_problem.return_flow(call, ret, ret_site,
                                                          func, fact,
                                                          zero_fact);
                                preserve_zero(return_facts, fact);
                                for (const Fact& rf : return_facts) {
                                    auto ret_ef =
                                        m_problem.return_edge_function(
                                            call, func, ret, ret_site, fact,
                                            rf);
                                    EdgeFunctionPtr ret_phi = make_edge_function(ret_ef);
                                    EdgeFunctionPtr composed = compose_cached(ret_phi, phi);
                                    // BUG (fixed): the old code created a self-loop path edge
                                    // PathEdge(ret_site, zero_fact, ret_site, rf).  This seeds
                                    // a brand-new analysis context rooted at ret_site, which is
                                    // wrong: the return should connect back to the *callee's*
                                    // entry context (entry, start_fact), not start a new one at
                                    // the return site.  The correct start node is the callee
                                    // entry and the correct start fact is start_fact (the fact
                                    // that was live at the callee entry under the seed context).
                                    on_return_transition(Node(ret, fact),
                                                         Node(ret_site, rf));
                                    add_jump_function(
                                        PathEdgeType(entry, start_fact, ret_site, rf),
                                        composed);
                                }
                            }
                        }
                    }
                }
            }

        } else {
            auto succs = m_graph_context.get_successors(curr);
            if (!succs.empty()) {
                for (const llvm::Instruction* succ : succs) {
                    FactSet next_facts = m_problem.normal_flow(curr, succ, fact);
                    preserve_zero(next_facts, fact);
                    for (const auto& tgt_fact : next_facts) {
                        NormalEdgeKey nkey(curr, succ, fact, tgt_fact);
                        EdgeFunctionPtr edge_fn;
                        if (m_config.enable_edge_function_caching()) {
                            auto eit = m_normal_edge_cache.find(nkey);
                            if (eit != m_normal_edge_cache.end()) {
                                edge_fn = eit->second;
                            } else {
                                edge_fn = make_edge_function(
                                    m_problem.normal_edge_function(curr, succ,
                                                                   fact,
                                                                   tgt_fact));
                                m_normal_edge_cache[nkey] = edge_fn;
                            }
                        } else {
                            edge_fn = make_edge_function(
                                m_problem.normal_edge_function(curr, succ, fact,
                                                               tgt_fact));
                        }
                        EdgeFunctionPtr new_phi = compose_cached(edge_fn, phi);
                        on_normal_transition(Node(curr, fact), Node(succ, tgt_fact));
                        add_jump_function(PathEdgeType(edge.start_node, start_fact,
                                                       succ, tgt_fact),
                                          new_phi);
                    }
                }
            }
        }

        m_steps_performed++;
    }

    if (!m_config.compute_values()) {
        if (m_config.enable_statistics()) {
            m_statistics.path_edges_total = m_path_edges.size();
            m_statistics.summary_edges_total = m_summary_edges.size();
            m_statistics.jump_functions_stored = m_jump_functions.size();
            m_statistics.values_computed = 0;
            m_statistics.end_time = std::chrono::steady_clock::now();
            m_statistics.total_time_seconds =
                std::chrono::duration_cast<std::chrono::duration<double>>(
                    m_statistics.end_time - m_statistics.start_time)
                    .count();
        }
        return;
    }

    // Phase 2: compute values using jump functions
    struct ValueEdge {
        const llvm::Instruction* target_node;
        Fact target_fact;
        EdgeFunctionPtr phi;
    };

    std::unordered_map<StartKey, std::vector<ValueEdge>, StartKeyHash> value_edges;

    for (const auto& entry : m_jump_functions) {
        const PathEdgeType& edge = entry.first;
        const EdgeFunctionPtr& phi = entry.second;
        StartKey key{edge.start_node, edge.start_fact};
        value_edges[key].push_back(ValueEdge{edge.target_node, edge.target_fact, phi});
    }

    for (const auto& entry : m_incoming) {
        const StartKey& callee_key = entry.first;
        for (const auto& incoming : entry.second) {
            const llvm::Function *callee = nullptr;
            const llvm::Instruction *callee_start = callee_key.start_node;
            if (callee_start != nullptr) {
                callee = callee_start->getFunction();
            }
            auto call_ef = m_problem.call_edge_function(
                incoming.call, callee, incoming.call_fact, callee_key.start_fact);
            EdgeFunctionPtr call_phi = make_edge_function(call_ef);
            // BUG (fixed): the old code used incoming.caller_phi directly.
            // caller_phi is the jump-function value at the time the incoming
            // edge was first recorded.  If the jump function for the caller
            // path edge was later updated (joined with a new path), the stored
            // caller_phi is stale and composing with it produces wrong values.
            // We must look up the *current* jump function from m_jump_functions.
            PathEdgeType caller_edge(incoming.start_node, incoming.start_fact,
                                     incoming.call, incoming.call_fact);
            auto jf_it = m_jump_functions.find(caller_edge);
            EdgeFunctionPtr current_caller_phi = (jf_it != m_jump_functions.end())
                                                 ? jf_it->second
                                                 : identity_func;
            EdgeFunctionPtr composed = compose_cached(call_phi, current_caller_phi);
            StartKey caller_key{incoming.start_node, incoming.start_fact};
            value_edges[caller_key].push_back(ValueEdge{callee_key.start_node,
                                                        callee_key.start_fact,
                                                        composed});
        }
    }

    auto update_value = [&](const llvm::Instruction* inst, const Fact& fact, const Value& incoming_value) {
        auto& fact_map = m_values[inst];
        auto current_it = fact_map.find(fact);
        Value current = (current_it != fact_map.end()) ? current_it->second : m_problem.bottom_value();
        Value joined = m_problem.join(current, incoming_value);
        if (current_it != fact_map.end() && joined == current) {
            return false;
        }
        fact_map[fact] = joined;
        return true;
    };

    std::vector<StartKey> value_worklist;
    for (const auto& pair : ide_seeds.get_seeds()) {
        const llvm::Instruction* entry = pair.first;
        auto seed_values = pair.second;
        FactSet facts;
        for (const auto& fv : seed_values) {
            facts.insert(fv.first);
        }
        if (m_problem.auto_add_zero()) {
            bool has_zero = false;
            for (const auto& fact : facts) {
                if (m_problem.is_zero_fact(fact)) {
                    has_zero = true;
                    break;
                }
            }
            if (!has_zero) {
                const Fact zero_fact = m_problem.zero_fact();
                facts.insert(zero_fact);
                seed_values[zero_fact] = m_problem.top_value();
            }
        }
        for (const auto& fact : facts) {
            auto it = seed_values.find(fact);
            Value seed_value =
                (it != seed_values.end()) ? it->second : m_problem.top_value();
            if (update_value(entry, fact, seed_value)) {
                value_worklist.push_back(StartKey{entry, fact});
            }
        }
    }

    while (!value_worklist.empty()) {
        StartKey key = value_worklist.back();
        value_worklist.pop_back();
        auto val_it = m_values.find(key.start_node);
        if (val_it == m_values.end()) {
            continue;
        }
        auto fact_it = val_it->second.find(key.start_fact);
        if (fact_it == val_it->second.end()) {
            continue;
        }
        const Value& start_value = fact_it->second;

        auto edge_it = value_edges.find(key);
        if (edge_it == value_edges.end()) {
            continue;
        }
        for (const auto& edge : edge_it->second) {
            Value result_val = (*edge.phi)(start_value);
            if (update_value(edge.target_node, edge.target_fact, result_val)) {
                value_worklist.push_back(StartKey{edge.target_node, edge.target_fact});
            }
        }
    }
    if (m_config.enable_statistics()) {
        m_statistics.path_edges_total = m_path_edges.size();
        m_statistics.summary_edges_total = m_summary_edges.size();
        m_statistics.jump_functions_stored = m_jump_functions.size();
        m_statistics.values_computed = m_values.size();
        m_statistics.end_time = std::chrono::steady_clock::now();
        m_statistics.total_time_seconds =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                m_statistics.end_time - m_statistics.start_time)
                .count();
    }
}

template<typename Problem>
typename IDESolver<Problem>::Value
IDESolver<Problem>::get_value_at(const llvm::Instruction* inst, const typename Problem::FactType& fact) const {
    auto inst_it = m_values.find(inst);
    if (inst_it != m_values.end()) {
        auto fact_it = inst_it->second.find(fact);
        if (fact_it != inst_it->second.end()) {
            return fact_it->second;
        }
    }
    return m_problem.bottom_value();
}

template<typename Problem>
typename IDESolver<Problem>::Value
IDESolver<Problem>::get_value_at_in_llvm_ssa(const llvm::Instruction* inst, const typename Problem::FactType& fact) const {
    if (inst->getType()->isVoidTy()) {
        return get_value_at(inst, fact);
    }
    if (const llvm::Instruction* next = inst->getNextNode()) {
        return get_value_at(next, fact);
    }
    if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(inst)) {
        llvm::BasicBlock* normal = invoke->getNormalDest();
        if (normal && !normal->empty()) {
            return get_value_at(&normal->front(), fact);
        }
    }
    return get_value_at(inst, fact);
}

template<typename Problem>
const std::unordered_map<const llvm::Instruction*,
                        std::unordered_map<typename Problem::FactType, typename Problem::ValueType>>&
IDESolver<Problem>::get_all_values() const {
    return m_values;
}

template<typename Problem>
void IDESolver<Problem>::get_path_edges(std::vector<PathEdgeType>& out_edges) const {
    out_edges.clear();
    out_edges.reserve(m_path_edges.size());
    for (const auto& edge : m_path_edges) {
        out_edges.push_back(edge);
    }
}

template<typename Problem>
void IDESolver<Problem>::get_summary_edges(
    std::vector<SummaryEdge<Fact>>& out_edges) const {
    out_edges.clear();
    out_edges.reserve(m_summary_edges.size());
    for (const auto& edge : m_summary_edges) {
        out_edges.push_back(edge);
    }
}

} // namespace ifds
