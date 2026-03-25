#ifndef ANALYSIS_DATAFLOW_WPDS_EXPLODEDWPDSBUILDER_H_
#define ANALYSIS_DATAFLOW_WPDS_EXPLODEDWPDSBUILDER_H_

/**
 * Builder for the paper's "exploded supergraph" WPDS encoding (Section 4).
 *
 * Control locations = dataflow variables (e.g. facts); stack symbols =
 * supergraph nodes. One rule per (from_control, from_stack) -> (to_control,
 * to_stack) with a micro-function weight. Supports normal edges (one stack
 * symbol on RHS) and call edges (two stack symbols: callee_entry, return_site).
 *
 * @see Reps, Schwoon, Jha: "Weighted Pushdown Systems and their Application
 *      to Interprocedural Dataflow Analysis", Section 4
 */

#include "Solvers/WPDS/WPDS.h"
#include "Solvers/WPDS/keys.h"
#include "Solvers/WPDS/semiring.h"

#include <functional>
#include <set>
#include <utility>
#include <vector>

namespace wpds {

/**
 * Builds a WPDS in the paper's exploded encoding: control states = variables,
 * stack symbols = supergraph nodes; one rule per (edge × (from_var, to_var)).
 *
 * @tparam T Semiring element type (e.g. GenKillTransformer or a micro-function
 * type).
 * @param wpds WPDS to add rules to (must already have semiring/query set).
 * @param semiring Semiring for zero() check and reference.
 * @param control_states Set of control state keys (e.g. Lambda + variable
 * keys).
 * @param normal_edges Pairs (from_stack, to_stack) for
 * intraprocedural/call-return edges.
 * @param call_edges Triples (from_stack, callee_entry_stack, return_site_stack)
 * for calls.
 * @param get_weight_normal Callback (from_control, from_stack, to_control,
 * to_stack) -> weight; return nullptr or zero to skip.
 * @param get_weight_call Callback (from_control, from_stack, to_control,
 * callee_entry, return_site) -> weight; return nullptr or zero to skip.
 */
template <typename T>
void buildExplodedWPDS(
    WPDS<T> &wpds, Semiring<T> &semiring,
    const std::set<wpds_key_t> &control_states,
    const std::vector<std::pair<wpds_key_t, wpds_key_t>> &normal_edges,
    const std::vector<std::tuple<wpds_key_t, wpds_key_t, wpds_key_t>>
        &call_edges,
    std::function<T *(wpds_key_t from_control, wpds_key_t from_stack,
                      wpds_key_t to_control, wpds_key_t to_stack)>
        get_weight_normal,
    std::function<T *(wpds_key_t from_control, wpds_key_t from_stack,
                      wpds_key_t to_control, wpds_key_t callee_entry,
                      wpds_key_t return_site)>
        get_weight_call) {
  typename Semiring<T>::SemiringElement zero_el = semiring.zero();
  T *zero_ptr = zero_el.get_ptr();

  for (wpds_key_t c : control_states) {
    wpds.add_element_to_P(c);
  }

  for (const auto &e : normal_edges) {
    wpds_key_t from_s = e.first;
    wpds_key_t to_s = e.second;
    for (wpds_key_t from_c : control_states) {
      for (wpds_key_t to_c : control_states) {
        T *w = get_weight_normal(from_c, from_s, to_c, to_s);
        if (w && !w->equal(zero_ptr)) {
          wpds.add_rule(from_c, from_s, to_c, to_s, w);
        }
      }
    }
  }

  for (const auto &tup : call_edges) {
    wpds_key_t from_s = std::get<0>(tup);
    wpds_key_t ce = std::get<1>(tup);
    wpds_key_t ret = std::get<2>(tup);
    for (wpds_key_t from_c : control_states) {
      for (wpds_key_t to_c : control_states) {
        T *w = get_weight_call(from_c, from_s, to_c, ce, ret);
        if (w && !w->equal(zero_ptr)) {
          wpds.add_rule(from_c, from_s, to_c, ce, ret, w);
        }
      }
    }
  }
}

} // namespace wpds

#endif // ANALYSIS_DATAFLOW_WPDS_EXPLODEDWPDSBUILDER_H_
