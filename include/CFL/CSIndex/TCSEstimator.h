/* Copyright (c) Hilmi Yildirim 2010,2011.

The software is provided on an as is basis for research purposes.
There is no additional support offered, nor are the author(s)
or their institutions liable under any circumstances.
*/
#ifndef _TCSEstimator_H_
#define _TCSEstimator_H_

#include "CFL/CSIndex/Graph.h"

class TCSEstimator {
public:
  TCSEstimator(Graph &g, int k);

private:
  void propagate_down(Graph &g, int *visited, int node, double val, int step,
                      double *avg);
  void find_mins(Graph &g, int *visited, int *reverse, double *avg, int step,
                 int totalsample);
};
#endif
