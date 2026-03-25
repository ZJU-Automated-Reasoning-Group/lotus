/* Copyright (C), 2016-present, Sourcebrella, Inc Ltd - All rights reserved.
 * Unauthorized copying, using, modifying of this file, via any medium is
 * strictly prohibited, proprietary, and confidential.
 *
 * Author:
 *   Qingkai Shi <qingkai.sqk@alibaba-inc.com>
 *
 * File Description:
 *
 *
 * Creation Date: 2021/7/10
 * Modification History:
 **/
#ifndef _TABULATION_H
#define _TABULATION_H

#include "CFL/CSIndex/AbstractQuery.h"
#include "CFL/CSIndex/Graph.h"

#include <set>

class Tabulation : public AbstractQuery {
private:
  Graph &vfg;
  std::set<int> visited;
  std::set<int> func_visited;

public:
  explicit Tabulation(Graph &g);

  bool reach(int s, int t) override;

  bool reach_func(int s, int t);

  bool is_call(int s, int t);

  bool is_return(int s, int t);

  double tc();

  void traverse(int s, std::set<int> &tc);

  void traverse_func(int s, std::set<int> &tc);

  const char *method() const override { return "Tabulate"; }

  void reset() override {
    visited.clear();
    func_visited.clear();
  }
};

#endif //_TABULATION_H
