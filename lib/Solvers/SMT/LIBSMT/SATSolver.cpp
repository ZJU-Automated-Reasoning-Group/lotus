/**
 * @file SATSolver.cpp
 * @brief Implementation of a modern DPLL-based SAT solver for Boolean formulas
 *
 * This file implements a conflict-driven clause learning (CDCL) SAT solver that
 * serves as a foundation for SMT solving through bit-blasting and other
 * techniques. It provides:
 * - Variable selection heuristics based on VSIDS (Variable State Independent
 * Decaying Sum)
 * - Conflict-driven learning and backtracking
 * - Restart strategies using the Luby sequence
 * - Efficient unit propagation through watched literals
 * - Phase selection based on variable activity
 * - Solution verification and reporting
 *
 * SAT solving is a critical component for bit-precise reasoning in the SMT
 * framework, especially for handling complex bit-vector operations, arrays, and
 * boolean combinations.
 *
 * Author: rainoftime
 * Creation Date: 2017
 */

#include "Solvers/SMT/LIBSMT/SATSolver.h"
// #include "Utils/ADT/PdQsort.h"
#include <algorithm>
// #include <functional>
// #include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define HALFLIFE 128
#define _DT 32 // RSAT phase selection threshold

struct comp_scores {
  variable *m_vars;

  comp_scores(variable *my_vars) : m_vars(my_vars) {}

  bool operator()(unsigned a, unsigned b) const { return SCORE(a) > SCORE(b); }
};

sat_solver::sat_solver(cnf &m_cnf) : cnf_manager(m_cnf) {
  // initialize parameters
  m_next_restart = m_luby.next() * (m_luby_unit = 512);
  m_next_decay = HALFLIFE;

  // assert_unit_clauses has failed
  if (m_d_level == 0)
    return;

  // assert pure literals
  for (int i = 1; i <= (int)m_vc; i++) {
    if (m_vars[i].m_value == _FREE) {
      if (m_vars[i].m_activity[_POSI] == 0 && m_vars[i].m_activity[_NEGA] > 0) {
        // m_ante is NULL, as opposed to empty clause for implied literals
        assert_literal(-i, NULL);
      } else if (m_vars[i].m_activity[_NEGA] == 0 &&
                 m_vars[i].m_activity[_POSI] > 0) {
        assert_literal(i, NULL);
      }
    }
  }
  // initialize m_var_order
  m_n_vars = 0;
  for (unsigned i = 1; i <= m_vc; i++) {
    if (m_vars[i].m_value == _FREE && SCORE(i) > 0) {
      m_var_order[m_n_vars++] = i;
      m_vars[i].m_phase =
          (m_vars[i].m_activity[_POSI] > m_vars[i].m_activity[_NEGA]) ? _POSI
                                                                      : _NEGA;
    }
  }
  sort(m_var_order, m_var_order + m_n_vars, comp_scores(m_vars));
  // pdqsort_branchless(m_var_order, m_var_order + m_n_vars,
  // comp_scores(m_vars));
  for (unsigned i = 0; i < m_n_vars; i++)
    m_var_position[m_var_order[i]] = i;
  m_next_var = 0;
  m_next_clause = m_clauses.size() - 1;
}

int sat_solver::select_literal() {
  unsigned x = 0;

  // pick best var in unsatisfied conflict clause nearest to top of stack
  // but only search 256 clauses
  int last_clause = m_next_clause > 256 ? (m_next_clause - 256) : 0;
  for (int i = m_next_clause; i >= last_clause; i--) {
    int *p = m_clauses[m_next_clause = i];

    // skip satisfied clauses
    bool sat = false;
    for (; (*p); p++) {
      if (SET(*p)) {
        sat = true;
        break;
      }
    }
    if (sat)
      continue;

    // traverse again, find best variable of clause
    int score = -1;
    for (p = m_clauses[i]; (*p); p++) {
      if (FREE(*p) && ((int)SCORE(VAR(*p))) > score) {
        x = VAR(*p);
        score = SCORE(x);
      }
    }

    // RSAT phase selection
    int d = m_vars[x].m_activity[_POSI] - m_vars[x].m_activity[_NEGA];
    if (d > _DT)
      return x;
    else if (-d > _DT)
      return -x;
    else
      return (m_vars[x].m_phase == _POSI) ? (x) : -(int)(x);
  }

  // fall back to VSIDS
  for (unsigned i = m_next_var; i < m_n_vars; i++) {
    if (m_vars[m_var_order[i]].m_value == _FREE) {
      x = m_var_order[i];
      m_next_var = i + 1;

      // RSAT phase selection
      int d = m_vars[x].m_activity[_POSI] - m_vars[x].m_activity[_NEGA];
      if (d > _DT)
        return x;
      else if (-d > _DT)
        return -x;
      else
        return (m_vars[x].m_phase == _POSI) ? (x) : -(int)(x);
    }
  }
  return 0;
}

bool sat_solver::run() {
  if (m_d_level == 0)
    return false;                            // assert_unit_clauses has failed
  for (int lit; (lit = select_literal());) { // pick decision literal
    if (!decide(lit))
      do {
        // decision/conflict
        // conflict has occurred in m_d_level 1, unsat
        if (m_a_level == 0)
          return false;

        // score decay
        if (m_n_conflicts == m_next_decay) {
          m_next_decay += HALFLIFE;
          score_decay();
        }

        // rewind to top of clause stack
        m_next_clause = m_clauses.size() - 1;

        // restart at m_d_level 1
        if (m_n_conflicts == m_next_restart) {
          m_n_restarts++;
          m_next_restart += m_luby.next() * m_luby_unit;
          backtrack(1);
          if (m_d_level != m_a_level)
            break;

          // partial restart at m_a_level
        } else
          backtrack(m_a_level);
      } while (!assert_cl()); // assert conflict literal
  }
  // TODO. add an option for disabling verify_solution()
  /*
   if (!verify_solution()) {
   printf("s unknown\n");
   exit(0);
   }
   */
  return true;
}

bool sat_solver::verify_solution() {
  int lit, *pool = m_lit_pools[0];
  for (unsigned i = 0; i < m_lit_pool_size_orig;) {
    bool satisfied = false;
    while ((lit = pool[i++])) {
      if (SET(lit)) {
        satisfied = true;
        while (pool[i++])
          ;
        break;
      }
    }
    if (!satisfied)
      return false;
  }
  return true;
}

void sat_solver::print_solution(FILE *ofp) {
  for (unsigned i = 1; i <= m_vc; i++) {
    if (m_vars[i].m_value == _POSI)
      fprintf(ofp, "%d ", i);
    else if (m_vars[i].m_value == _NEGA)
      fprintf(ofp, "-%d ", i);
  }
  fprintf(ofp, "0\n");
}

void sat_solver::print_stats() {
  printf("c %d decisions, %d conflicts, %d restarts\n", m_n_decisions,
         m_n_conflicts, m_n_restarts);
}
