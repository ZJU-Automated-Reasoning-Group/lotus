/* Copyright (c) Hilmi Yildirim 2010,2011.

The software is provided on an as is basis for research purposes.
There is no additional support offered, nor are the author(s)
or their institutions liable under any circumstances.
*/

#include "CFL/CSIndex/FLARE/Grail/TCSEstimator.h"

#include <random>

namespace lotus::cfl::cs_index::flare::grail {

TCSEstimator::TCSEstimator(Graph &g, int k) {
  int cnt = g.num_vertices();
  double *avg = new double[cnt];
  int *visited = new int[cnt];
  int *reverse = new int[cnt];
  vector<int> _index;
  for (int i = 0; i < cnt; i++) {
    _index.push_back(i);
    avg[i] = 0;
    visited[i] = -1;
  }

  for (int i = 0; i < k; i++) {
    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(_index.begin(), _index.end(), rng);
    for (int j = 0; j < cnt; j++) {
      reverse[_index[j]] = j;
    }
    find_mins(g, visited, reverse, avg, i, k);
  }

  for (int i = 0; i < cnt; i++) {
    if (avg[i] == 0)
      g[i].tcs = 0;
    else
      g[i].tcs = (cnt / avg[i]) - 1;
    // cout << "tcs[" << i<<"] = " << g[i].tcs << " avg " << avg[i] << '\n';
    g[i].mingap = cnt;
  }
}

void TCSEstimator::propagate_down(Graph &g, int *visited, int node, double val,
                                  int step, double *avg) {
  EdgeList el = g.in_edges(node);
  EdgeList::iterator eit;
  for (eit = el.begin(); eit != el.end(); eit++) {
    if (visited[*eit] != step) {
      avg[*eit] += val;
      visited[*eit] = step;
      TCSEstimator::propagate_down(g, visited, *eit, val, step, avg);
    }
  }
}

void TCSEstimator::find_mins(Graph &g, int *visited, int *reverse, double *avg,
                             int step, int totalsample) {
  int n = g.num_vertices();
  for (int i = 0; i < n; i++) {
    if (visited[reverse[i]] != step) {
      TCSEstimator::propagate_down(g, visited, reverse[i],
                                   (double(i + 1)) / totalsample, step, avg);
    }
  }
}

} // namespace lotus::cfl::cs_index::flare::grail
