/*
 * Author: rainoftime
 * File Description: CNF Manager
 * Creation Date:  2017.
 * Modification History:
 */

#pragma once

#include <deque>
#include <vector>

#include <math.h>
#include <stddef.h>

using namespace std;

#define _NEGA 0 // variable values are negative
#define _POSI 1 // positive, or
#define _FREE 2 // free

#define SIGN(lit) (((lit) > 0) ? _POSI : _NEGA)
#define VAR(lit) (abs(lit))
#define NEG(lit) (-(lit))

#define FREE(lit) (m_vars[VAR(lit)].m_value == _FREE)
#define SET(lit) (m_vars[VAR(lit)].m_value == SIGN(lit))
#define RESOLVED(lit) (m_vars[VAR(lit)].m_value == SIGN(NEG(lit)))
#define IMPLIST(lit) (m_vars[VAR(lit)].m_imp[SIGN(lit)])
#define WATCHLIST(lit) (m_vars[VAR(lit)].m_watch[SIGN(lit)])
#define SCORE(var) (m_vars[(var)].m_activity[0] + m_vars[(var)].m_activity[1])

struct cnf {
  unsigned m_vc;   // var count
  unsigned m_cc;   // clause count
  int **m_clauses; // 2-dim. array with entries same as in cnf file
  unsigned m_lc;   // literal count
  unsigned *m_cl;  // clause length
  cnf(char *fname);

  ~cnf();
};

struct variable {
  bool m_mark;              // used in 1-UIP derivation
  bool m_phase;             // suggested phase for decision
  char m_value;             // _POSI, _NEGA, _FREE
  unsigned m_d_level;       // decision level where var is set
  int *m_ante;              // antecedent clause if implied
  unsigned m_activity[2];   // scores for literals
  int *m_imp[2];            // implication lists for binary clauses
  vector<int *> m_watch[2]; // watch lists for other clauses
  variable() : m_mark(false), m_value(_FREE) {
    m_activity[0] = m_activity[1] = 0;
    m_watch[0].clear();
    m_watch[1].clear();
  };
};

class cnf_manager {
protected:
  unsigned m_vc;            // variable count
  variable *m_vars;         // array of variables
  unsigned *m_var_order;    // variables ordered by score
  unsigned *m_var_position; // variable position in m_var_order
  unsigned m_next_var;      // starting point in m_var_order

  int *m_lit_pool;               // array of literals as in clauses
  unsigned m_lit_pool_size;      // literal pool size
  unsigned m_lit_pool_size_orig; // original clauses only
  unsigned m_lit_pool_capacity;  // capacity of current m_lit_pool
  vector<int *> m_lit_pools;     // all m_lit_pools created
  vector<int *> m_clauses;       // array of conflict clauses
  int m_next_clause; // starting point to look for unsatisfied conflict clause

  int *m_stack_top;               // decision/implication stack
  unsigned m_a_level;             // assertion level
  unsigned m_d_level;             // decision level
  unsigned m_n_decisions;         // num of decisions
  unsigned m_n_conflicts;         // num of conflicts
  unsigned m_n_restarts;          // num of restarts
  deque<int> m_conflict_lits;     // stores conflict literals
  deque<int> m_tmp_conflict_lits; // ditto, temporary
  int *m_conflict_clause;         // points to learned clause in m_lit_pool

  void set_literal(int lit, int *ante); // set value, ante, level
  bool assert_literal(int lit,
                      int *ante); // set literal and perform unit propagation
  bool assert_unit_clauses();     // assert initial unit clauses
  bool decide(int lit);           // increment m_d_level and call assert_literal
  void learn_clause(int *first_lit); // store learned clause in m_conflict_lits
                                     // and call add_clause
  void add_clause(); // add m_conflict_lits to  m_lit_pool and set up watches
  bool assert_cl();  // assert literal implied by conflict clause
  void backtrack(unsigned level); // undo assignments in levels > level
  void score_decay();             // divide scores by constant
  void update_scores(int *first); // update variable scores and positions
public:
  cnf_manager() {};
  cnf_manager(cnf &m_cnf);
  ~cnf_manager();
};

inline bool cnf_manager::assert_cl() {
  return assert_literal(*m_conflict_clause, m_conflict_clause + 1);
}

inline bool cnf_manager::decide(int lit) {
  m_n_decisions++;
  m_d_level++;
  return assert_literal(lit, NULL);
}

inline void cnf_manager::backtrack(unsigned b_level) {
  for (unsigned var;
       m_vars[var = VAR(*(m_stack_top - 1))].m_d_level > b_level;) {
    if (m_vars[var].m_d_level < m_d_level)
      m_vars[var].m_phase = m_vars[var].m_value;
    m_vars[var].m_value = _FREE;
    if (m_var_position[var] < m_next_var)
      m_next_var = m_var_position[var];
    m_stack_top--;
  }
  m_d_level = b_level;
}

inline void cnf_manager::score_decay() {
  // this may slightly disturb var order
  // e.g., (7 + 7) => (3 + 3) whereas (6 + 8) => (3 + 4)
  for (unsigned i = 1; i <= m_vc; i++) {
    m_vars[i].m_activity[0] >>= 1;
    m_vars[i].m_activity[1] >>= 1;
  }
}
