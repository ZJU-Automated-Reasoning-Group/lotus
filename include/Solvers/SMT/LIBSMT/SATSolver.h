/*
 * Author: rainoftime
 * File Description: CNF Manager
 * Creation Date:  2017.
 * Modification History:
 */

#pragma once

#include "Solvers/SMT/LIBSMT/CNF.h"

#include <stdio.h>

struct luby {             // restart scheduler as proposed in
  vector<unsigned> m_seq; // Optimal Speedup of Las Vegas Algorithms
  unsigned m_index;       // Michael Luby et al, 1993
  unsigned m_k;

  luby() : m_index(0), m_k(1) {}

  unsigned next() {
    if (++m_index == (unsigned)((1 << m_k) - 1)) {
      m_seq.push_back(1 << (m_k++ - 1));
    } else {
      m_seq.push_back(m_seq[m_index - (1 << (m_k - 1))]);
    }
    return m_seq.back();
  }
};

class sat_solver : public cnf_manager {
  unsigned m_n_vars;       // num of variables in m_var_order
  luby m_luby;             // restart scheduler
  unsigned m_luby_unit;    // unit run length for Luby's
  unsigned m_next_decay;   // next score decay point
  unsigned m_next_restart; // next restart point

  int select_literal();

  bool verify_solution();

public:
  sat_solver() {};

  sat_solver(cnf &m_cnf);

  bool run();

  void print_stats();

  void print_solution(FILE *);
};
