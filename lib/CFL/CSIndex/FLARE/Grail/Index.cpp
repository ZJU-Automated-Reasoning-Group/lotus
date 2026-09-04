/* Copyright (c) Hilmi Yildirim 2010,2011.

The software is provided on an as is basis for research purposes.
There is no additional support offered, nor are the author(s)
or their institutions liable under any circumstances.
*/
#include "CFL/CSIndex/FLARE/Grail/Index.h"

#include "CFL/CSIndex/FLARE/Grail/TCSEstimator.h"

#include <cstdlib>
#include <iostream>
#include <queue>
#include <random>

namespace lotus::cfl::cs_index::flare::grail {

vector<int> _index;         ///< Global index vector for labeling
vector<double> customIndex; ///< Global custom index vector

/**
 * @brief Comparator for index-based sorting.
 * @tparam T Array type
 */
template <class T> struct index_cmp {
  index_cmp(const T arr) : arr(arr) {}
  /**
   * @brief Compare two indices based on array values.
   * @param a First index
   * @param b Second index
   * @return true if arr[a] < arr[b]
   */
  bool operator()(const size_t a, const size_t b) const {
    return arr[a] < arr[b];
  }
  const T arr;
};

/**
 * @brief Custom comparator for sorting.
 * @tparam T Array type
 */
template <class T> struct custom_cmp {
  custom_cmp(const T arr) : arr(arr) {}
  /**
   * @brief Compare two indices based on array values (descending order).
   * @param a First index
   * @param b Second index
   * @return true if arr[a] > arr[b]
   */
  bool operator()(const size_t a, const size_t b) const {
    return arr[a] > arr[b];
  }
  const T arr;
};

/*******************************************************************************************
GRAIL LABELING :
                1- Constructor:
                2- SetIndex - an auxillary function that is used by fixed
reverse pairs 3- fixedreverselabeling - labels with fixed reverse random
ordering 4- randomlabeling - random ordering 5- mingaplabeling - heuristics 6-
fixedreversevisit - used by fixedreverselabeling 7- visit - used by random
labeling 8- mingapvisit - used by min gap labeling
*******************************************************************************************/

/**
 * @brief Constructor with graph and parameters.
 * @param graph Reference to the main graph
 * @param Dim Dimension for labeling
 * @param labelingType Type of labeling to use
 * @param pool Pool usage flag
 * @param poolsize Pool size
 *
 * Initializes a GRAIL object with the specified parameters and performs
 * labeling.
 */
Index::Index(Graph &graph, int Dim, int labelingType, bool pool, int poolsize)
    : g(graph), dim(Dim), POOL(pool), POOLSIZE(poolsize) {
  int i, maxid = g.num_vertices();
  visited = new int[maxid];
  QueryCnt = 0;
  if (labelingType >= 2) {
    TCSEstimator tcse(graph, 100);
  }
  for (i = 0; i < maxid; i++) {
    graph[i].pre = new vector<int>();
    graph[i].post = new vector<int>();
    graph[i].middle = new vector<int>();
    visited[i] = -1;
  }
  if (!POOL) {
    POOLSIZE = dim;
  }
  for (i = 0; i < POOLSIZE; i++) {
    switch (labelingType) {
    case 0:
      Index::randomlabeling(graph);
      break;
    case 1:
      Index::setIndex(graph, i);
      Index::fixedreverselabeling(graph, i);
      break;
    default:
      Index::setCustomIndex(graph, i, labelingType);
      Index::customlabeling(graph, i);
      break;
    }
    cout << "Labeling " << i << " is completed\n";
    /*		for( int k = 0 ; k < maxid; k++){
                            cout << k << "["<<(graph[k].pre)->at(i) <<
       ","<<(graph[k].post)->at(i) << "] ";
                    }
                    cout << '\n';
    */
  }
  PositiveCut = NegativeCut = TotalCall = TotalDepth = CurrentDepth = 0;
}

Index::~Index() {}

void Index::set_level_filter(bool lf) { LEVEL_FILTER = lf; }

void Index::setIndex(Graph &g, int traversal) {
  if (traversal == 0) {
    int cnt = g.num_vertices();
    for (int i = 0; i < cnt; i++) {
      _index.push_back(i);
    }
  } else if (traversal % 2 == 0) {
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(_index.begin(), _index.end(), g);
  }
}

void Index::setCustomIndex(Graph &g, int traversal, int type) {
  int cnt = g.num_vertices();
  if (traversal == 0) {
    for (int i = 0; i < cnt; i++) {
      customIndex.push_back(g.tcs(i));
      //	customIndex.push_back(0);
    }
  } else if (traversal == 1) {
    if (type < 4) {
      cout << "A\n";
      for (int i = 0; i < cnt; i++) {
        customIndex[i] = g[i].post->at(0) - g[i].pre->at(0);
      }
    } else {
      for (int i = 0; i < cnt; i++) {
        customIndex[i] = g[i].post->at(0) - g[i].pre->at(0) - g.tcs(i);
      }
    }
  } else {
    for (int i = 0; i < cnt; i++) {
      switch (type) {
      case 2:
        customIndex[i] *=
            g[i].post->at(traversal - 1) - g[i].pre->at(traversal - 1);
        break;
      case 3:
        customIndex[i] =
            min(customIndex[i], (double)g[i].post->at(traversal - 1) -
                                    g[i].pre->at(traversal - 1));
        break;
      case 4:
        customIndex[i] *= g[i].post->at(traversal - 1) -
                          g[i].pre->at(traversal - 1) - g.tcs(i);
        if (customIndex[i] < 0)
          customIndex[i] = 0;
        break;
      case 5:
        customIndex[i] =
            min(customIndex[i], g[i].post->at(traversal - 1) -
                                    g[i].pre->at(traversal - 1) - g.tcs(i));
        break;
      }
    }
  }
  /*		cout << "Traversal " << traversal << " ";
                  for(int i=0; i<cnt; i++){
                          cout << customIndex[i] << " " ;
                  }
                  cout << '\n';
  */
}

// compute interval label for each node of tree (pre_order, post_order)
void Index::fixedreverselabeling(Graph &tree, int traversal) {
  vector<int> roots = tree.getRoots();
  vector<int>::iterator sit;
  int pre_post = 0;
  vector<bool> visited(tree.num_vertices(), false);
  sort(roots.begin(), roots.end(), index_cmp<vector<int> &>(_index));
  if (traversal % 2)
    reverse(roots.begin(), roots.end());

  for (sit = roots.begin(); sit != roots.end(); sit++) {
    pre_post++;
    fixedreversevisit(tree, *sit, pre_post, visited, traversal);
  }
}

// compute interval label for each node of tree (pre_order, post_order)
void Index::customlabeling(Graph &graph, int traversal) {
  vector<int> roots = graph.getRoots();
  vector<int>::iterator sit;
  int pre_post = 0;
  vector<bool> visited(graph.num_vertices(), false);

  /*	cout << " Sorting roots - before " ;
          for (sit = roots.begin(); sit != roots.end(); sit++) {
                  cout << " customIndex[" << *sit<<"]=" << customIndex[*sit] <<
     " " ; } cout << '\n';
  */

  sort(roots.begin(), roots.end(), custom_cmp<vector<double> &>(customIndex));

  /*	cout << " Sorting roots - before " ;
          for (sit = roots.begin(); sit != roots.end(); sit++) {
                  cout << " customIndex[" << *sit<<"]=" << customIndex[*sit] <<
     " " ; } cout << '\n';
  */

  for (sit = roots.begin(); sit != roots.end(); sit++) {
    pre_post++;
    customvisit(graph, *sit, pre_post, visited, traversal);
  }
}

// traverse tree to label node with pre and post order by giving a start node
int Index::customvisit(Graph &tree, int vid, int &pre_post,
                       vector<bool> &visited, int traversal) {
  visited[vid] = true;
  EdgeList el = tree.out_edges(vid);
  EdgeList::iterator eit;

  /*	cout << " Sorting children of " << vid << "  - before " ;
          for (eit = el.begin(); eit != el.end(); eit++) {
                  cout << " customIndex[" << *eit<<"]=" << customIndex[*eit] <<
     " " ; } cout << '\n';
  */
  sort(el.begin(), el.end(), custom_cmp<vector<double> &>(customIndex));

  /*
          cout << " Sorting children of " << vid << "  - after " ;
          for (eit = el.begin(); eit != el.end(); eit++) {
                  cout << " customIndex[" << *eit<<"]=" << customIndex[*eit] <<
     " " ; } cout << '\n';
  */
  int pre_order = tree.num_vertices() + 1;
  tree[vid].middle->push_back(pre_post);
  for (eit = el.begin(); eit != el.end(); eit++) {
    if (!visited[*eit]) {
      pre_order =
          min(pre_order, customvisit(tree, *eit, pre_post, visited, traversal));
    } else
      pre_order = min(pre_order, tree[*eit].pre->back());
  }

  pre_order = min(pre_order, pre_post);
  tree[vid].pre->push_back(pre_order);
  tree[vid].post->push_back(pre_post);
  if (pre_post - pre_order < tree[vid].mingap) {
    tree[vid].mingap = pre_post - pre_order;
  }
  pre_post++;
  //	cout << "exiting " << vid << '\n';
  return pre_order;
}

// compute interval label for each node of tree (pre_order, post_order)
void Index::randomlabeling(Graph &tree) {
  vector<int> roots = tree.getRoots();
  vector<int>::iterator sit;
  int pre_post = 0;
  vector<bool> visited(tree.num_vertices(), false);
  std::random_device rd;
  std::mt19937 g(rd());
  std::shuffle(roots.begin(), roots.end(), g);
  for (sit = roots.begin(); sit != roots.end(); sit++) {
    pre_post++;
    visit(tree, *sit, pre_post, visited);
  }
}

// traverse tree to label node with pre and post order by giving a start node
int Index::visit(Graph &tree, int vid, int &pre_post, vector<bool> &visited) {
  //	cout << "entering " << vid << '\n';
  visited[vid] = true;
  EdgeList el = tree.out_edges(vid);
  std::random_device rd;
  std::mt19937 g(rd());
  std::shuffle(el.begin(), el.end(), g);
  EdgeList::iterator eit;
  int pre_order = tree.num_vertices() + 1;
  tree[vid].middle->push_back(pre_post);
  for (eit = el.begin(); eit != el.end(); eit++) {
    if (!visited[*eit]) {
      pre_order = min(pre_order, visit(tree, *eit, pre_post, visited));
    } else
      pre_order = min(pre_order, tree[*eit].pre->back());
    // pre_order=min(pre_order,tree[*eit].pre->at(tree[*eit].pre->size()-1));
  }

  pre_order = min(pre_order, pre_post);
  tree[vid].pre->push_back(pre_order);
  tree[vid].post->push_back(pre_post);
  pre_post++;
  return pre_order;
}

// traverse tree to label node with pre and post order by giving a start node
int Index::fixedreversevisit(Graph &tree, int vid, int &pre_post,
                             vector<bool> &visited, int traversal) {
  //	cout << "entering " << vid << '\n';
  visited[vid] = true;
  EdgeList el = tree.out_edges(vid);
  sort(el.begin(), el.end(), index_cmp<vector<int> &>(_index));
  if (traversal % 2)
    reverse(el.begin(), el.end());
  EdgeList::iterator eit;
  int pre_order = tree.num_vertices() + 1;
  tree[vid].middle->push_back(pre_post);
  for (eit = el.begin(); eit != el.end(); eit++) {
    if (!visited[*eit]) {
      pre_order = min(pre_order, fixedreversevisit(tree, *eit, pre_post,
                                                   visited, traversal));
    } else
      pre_order = min(pre_order, tree[*eit].pre->back());
    // pre_order=min(pre_order,tree[*eit].pre->at(tree[*eit].pre->size()-1));
  }

  pre_order = min(pre_order, pre_post);
  tree[vid].pre->push_back(pre_order);
  tree[vid].post->push_back(pre_post);
  pre_post++;
  //	cout << "exiting " << vid << '\n';
  return pre_order;
}

/*************************************************************************************
GRAIL Query Functions
*************************************************************************************/
bool Index::contains(int src, int trg) {
  if (g[src].pre->size() == 0 || g[trg].pre->size() == 0 ||
      g[src].post->size() == 0 || g[trg].post->size() == 0) {
    return false;
  }
  //	std::cout << g[src].pre->size() << '\n';
  //	std::cout << g[src].post->size() << '\n';
  //	std::cout << g[trg].pre->size() << '\n';
  //	std::cout << g[trg].post->size() << '\n';

  //	for(int i = 0; i < g[src].pre->size(); i++){
  //		std::cout << g[src].pre->at(i) << '\n';
  //	}
  //	cout << '\n';
  //	for(int i = 0; i < g[trg].pre->size(); i++){
  //		std::cout << g[trg].pre->at(i) << '\n';
  //	}
  //	cout << '\n';
  //
  //	for(int i = 0; i < g[src].post->size(); i++){
  //		std::cout << g[src].post->at(i) << '\n';
  //	}
  //	cout << '\n';
  //	for(int i = 0; i < g[trg].post->size(); i++){
  //		std::cout << g[trg].post->at(i) << '\n';
  //	}

  int i, j;
  if (POOL) {
    for (i = 0; i < dim; i++) {
      j = rand() % POOLSIZE;
      if (g[src].pre->at(j) > g[trg].pre->at(j)) {
#ifdef DEBUG
        NegativeCut++;
#endif
        return false;
      }
      if (g[src].post->at(j) < g[trg].post->at(j)) {
#ifdef DEBUG
        NegativeCut++;
#endif
        return false;
      }
    }
  } else {
    for (i = 0; i < dim; i++) {
      if (g[src].pre->at(i) > g[trg].pre->at(i)) {
#ifdef DEBUG
        NegativeCut++;
#endif
        return false;
      }
      if (g[src].post->at(i) < g[trg].post->at(i)) {
#ifdef DEBUG
        NegativeCut++;
#endif
        return false;
      }
    }
  }
  return true;
}

int Index::containsPP(int src, int trg) {
  int i, j;

  if (POOL) {
    for (i = 0; i < dim; i++) {
      j = rand() % POOLSIZE;
      if (g[src].pre->at(j) > g[trg].pre->at(j))
        return -1;
      if (g[src].post->at(j) < g[trg].post->at(j))
        return -1;
      if (g[src].middle->at(j) < g[trg].post->at(j))
        return 1;
    }
  } else {
    for (i = 0; i < dim; i++) {
      if (g[src].pre->at(i) > g[trg].pre->at(i))
        return -1;
      if (g[src].post->at(i) < g[trg].post->at(i))
        return -1;
      if (g[src].middle->at(i) < g[trg].post->at(i))
        return 1;
    }
  }
  return 0;
}

bool Index::go_for_reach(int src, int trg) {
#ifdef DEBUG
  TotalCall++;
#endif
  if (src == trg)
    return true;

  visited[src] = QueryCnt;
  EdgeList el = g.out_edges(src);
  EdgeList::iterator eit;

  for (eit = el.begin(); eit != el.end(); eit++) {
    if (visited[*eit] != QueryCnt && contains(*eit, trg)) {
      if (go_for_reach(*eit, trg)) {
        return true;
      }
    }
  }
  return false;
}

bool Index::go_for_reach_lf(int src, int trg) {
#ifdef DEBUG
  TotalCall++;
#endif
  if (src == trg)
    return true;

  if (g[src].top_level >= g[trg].top_level) // if using level filter, reject if
                                            // in a higher topological level
    return false;

  visited[src] = QueryCnt;
  EdgeList el = g.out_edges(src);
  EdgeList::iterator eit;

  for (eit = el.begin(); eit != el.end(); eit++) {
    if (visited[*eit] != QueryCnt && contains(*eit, trg)) {
      if (go_for_reach_lf(*eit, trg)) {
        return true;
      }
    }
  }
  return false;
}

bool Index::go_for_reachPP_lf(int src, int trg) {
  int res;
#ifdef DEBUG
  TotalCall++;
  CurrentDepth++;
#endif
  if (src == trg)
    return true;

  if (g[src].top_level >= g[trg].top_level) // if using level filter, reject if
                                            // in a higher topological level
    return false;

  visited[src] = QueryCnt;
  EdgeList el = g.out_edges(src);
  EdgeList::iterator eit;

  for (eit = el.begin(); eit != el.end(); eit++) {
    if (visited[*eit] != QueryCnt) {
      res = containsPP(*eit, trg);
      switch (res) {
      case 1:
#ifdef DEBUG
        TotalDepth += CurrentDepth;
        PositiveCut++;
#endif
        return true;
      case 0:
        if (go_for_reachPP_lf(*eit, trg))
          return true;
        break;
      case -1:
#ifdef DEBUG
        NegativeCut++;
#endif
        break;
      }
    }
  }
#ifdef DEBUG
  CurrentDepth--;
#endif
  return false;
}

bool Index::go_for_reachPP(int src, int trg) {
  int res;
#ifdef DEBUG
  TotalCall++;
  CurrentDepth++;
#endif
  if (src == trg)
    return true;

  visited[src] = QueryCnt;
  EdgeList el = g.out_edges(src);
  EdgeList::iterator eit;

  for (eit = el.begin(); eit != el.end(); eit++) {
    if (visited[*eit] != QueryCnt) {
      res = containsPP(*eit, trg);
      switch (res) {
      case 1:
#ifdef DEBUG
        TotalDepth += CurrentDepth;
        PositiveCut++;
#endif
        return true;
      case 0:
        if (go_for_reachPP(*eit, trg))
          return true;
        break;
      case -1:
#ifdef DEBUG
        NegativeCut++;
#endif
        break;
      }
    }
  }
#ifdef DEBUG
  CurrentDepth--;
#endif
  return false;
}

bool Index::bidirectionalReachPP(int src, int trg, ExceptionList *exclist) {
  int tmp;
  queue<int> forward;
  queue<int> backward;
  if (src == trg)
    return true;

  tmp = containsPP(src, trg);
  if (tmp == -1) { // if it does not contain reject
#ifdef DEBUG
    NegativeCut++;
#endif
    return false;
  } else if (tmp == 1) {
#ifdef DEBUG
    PositiveCut++;
#endif
    return true;
  }

  if (exclist != NULL) {                  // if using exception lists
    if (exclist->isAnException(src, trg)) // if it is an exception, reject
      return false;
    else
      return true;
  }

  QueryCnt++;
  visited[src] = QueryCnt;
  forward.push(src);
  visited[trg] = -QueryCnt;
  backward.push(trg);

  EdgeList el;
  vector<int>::iterator ei;
  int next;
  while (!forward.empty() && !backward.empty()) {

    next = forward.front();
    forward.pop();
    el = g.out_edges(next);
    // for each child of start node
    for (ei = el.begin(); ei != el.end(); ei++) {
      if (visited[*ei] == -QueryCnt) {
        return true;
      } else if (visited[*ei] != QueryCnt) {
        visited[*ei] = QueryCnt;
        tmp = containsPP(*ei, trg);
        switch (tmp) {
        case 1:
#ifdef DEBUG
          PositiveCut++;
#endif
          return true;
        case -1:
#ifdef DEBUG
          NegativeCut++;
#endif
          break;
        case 0:
          forward.push(*ei);
        }
      }
    }

    next = backward.front();
    backward.pop();
    el = g.in_edges(next);

    for (ei = el.begin(); ei != el.end(); ei++) {
      if (visited[*ei] == QueryCnt) {
        return true;
      } else if (visited[*ei] != -QueryCnt) {
        visited[*ei] = -QueryCnt;
        tmp = containsPP(src, *ei);
        switch (tmp) {
        case 1:
#ifdef DEBUG
          PositiveCut++;
#endif
          return true;
        case -1:
#ifdef DEBUG
          NegativeCut++;
#endif
          break;
        case 0:
          backward.push(*ei);
        }
      }
    }
  }
  return false;
}

bool Index::bidirectionalReachPP_lf(int src, int trg, ExceptionList *exclist) {
  int tmp;
  queue<int> forward;
  queue<int> backward;
  if (src == trg)
    return true;

  tmp = containsPP(src, trg);
  if (tmp == -1) { // if it does not contain reject
#ifdef DEBUG
    NegativeCut++;
#endif
    return false;
  } else if (tmp == 1) {
#ifdef DEBUG
    PositiveCut++;
#endif
    return true;
  }

  if (exclist != NULL) {                  // if using exception lists
    if (exclist->isAnException(src, trg)) // if it is an exception, reject
      return false;
    else if (g[src].top_level >=
             g[trg].top_level) // if using level filter, reject if in a higher
                               // topological level
      return false;
    else
      return true;
  }

  QueryCnt++;
  visited[src] = QueryCnt;
  forward.push(src);
  visited[trg] = -QueryCnt;
  backward.push(trg);

  EdgeList el;
  vector<int>::iterator ei;
  int next;
  while (!forward.empty() && !backward.empty()) {

    next = forward.front();
    forward.pop();
    el = g.out_edges(next);
    // for each child of start node
    if (g[next].top_level < g[trg].top_level) {
      for (ei = el.begin(); ei != el.end(); ei++) {
        if (visited[*ei] == -QueryCnt) {
          return true;
        } else if (visited[*ei] != QueryCnt) {
          visited[*ei] = QueryCnt;
          tmp = containsPP(*ei, trg);
          switch (tmp) {
          case 1:
#ifdef DEBUG
            PositiveCut++;
#endif
            return true;
          case -1:
#ifdef DEBUG
            NegativeCut++;
#endif
            break;
          case 0:
            forward.push(*ei);
          }
        }
      }
    }

    next = backward.front();
    backward.pop();
    el = g.in_edges(next);

    if (g[src].top_level < g[next].top_level) {
      for (ei = el.begin(); ei != el.end(); ei++) {
        if (visited[*ei] == QueryCnt) {
          return true;
        } else if (visited[*ei] != -QueryCnt) {
          visited[*ei] = -QueryCnt;
          tmp = containsPP(src, *ei);
          switch (tmp) {
          case 1:
#ifdef DEBUG
            PositiveCut++;
#endif
            return true;
          case -1:
#ifdef DEBUG
            NegativeCut++;
#endif
            break;
          case 0:
            backward.push(*ei);
          }
        }
      }
    }
  }
  return false;
}

bool Index::bidirectionalReach(int src, int trg, ExceptionList *exclist) {
  queue<int> forward;
  queue<int> backward;
  if (src == trg)
    return true;

  if (!contains(src, trg)) // if it does not contain reject
    return false;
  if (exclist != NULL) {                  // if using exception lists
    if (exclist->isAnException(src, trg)) // if it is an exception, reject
      return false;
    else
      return true;
  }
  QueryCnt++;
  visited[src] = QueryCnt;
  forward.push(src);
  visited[trg] = -QueryCnt;
  backward.push(trg);

  EdgeList el;
  vector<int>::iterator ei;
  int next;
  while (!forward.empty() && !backward.empty()) {

    next = forward.front();
    forward.pop();
    el = g.out_edges(next);
    // for each child of start node
    for (ei = el.begin(); ei != el.end(); ei++) {
      if (visited[*ei] == -QueryCnt) {
        return true;
      } else if (visited[*ei] != QueryCnt && contains(*ei, trg)) {
        forward.push(*ei);
        visited[*ei] = QueryCnt;
      }
    }

    next = backward.front();
    backward.pop();
    el = g.in_edges(next);

    for (ei = el.begin(); ei != el.end(); ei++) {
      if (visited[*ei] == QueryCnt) {
        return true;
      } else if (visited[*ei] != -QueryCnt && contains(src, *ei)) {
        backward.push(*ei);
        visited[*ei] = -QueryCnt;
      }
    }
  }
  return false;
}

bool Index::bidirectionalReach_lf(int src, int trg, ExceptionList *exclist) {
  queue<int> forward;
  queue<int> backward;
  if (src == trg)
    return true;

  if (!contains(src, trg)) // if it does not contain reject
    return false;
  if (exclist != NULL) {                  // if using exception lists
    if (exclist->isAnException(src, trg)) // if it is an exception, reject
      return false;
    else if (g[src].top_level >=
             g[trg].top_level) // if using level filter, reject if in a higher
                               // topological level
      return false;
    else
      return true;
  }
  QueryCnt++;
  visited[src] = QueryCnt;
  forward.push(src);
  visited[trg] = -QueryCnt;
  backward.push(trg);

  EdgeList el;
  vector<int>::iterator ei;
  int next;
  while (!forward.empty() && !backward.empty()) {

    next = forward.front();
    forward.pop();
    el = g.out_edges(next);
    // for each child of start node
    if (g[next].top_level < g[trg].top_level) {
      for (ei = el.begin(); ei != el.end(); ei++) {
        if (visited[*ei] == -QueryCnt) {
          return true;
        } else if (visited[*ei] != QueryCnt && contains(*ei, trg)) {
          forward.push(*ei);
          visited[*ei] = QueryCnt;
        }
      }
    }

    next = backward.front();
    backward.pop();
    el = g.in_edges(next);

    if (g[src].top_level < g[next].top_level) {
      for (ei = el.begin(); ei != el.end(); ei++) {
        if (visited[*ei] == QueryCnt) {
          return true;
        } else if (visited[*ei] != -QueryCnt && contains(src, *ei)) {
          backward.push(*ei);
          visited[*ei] = -QueryCnt;
        }
      }
    }
  }
  return false;
}

// bool Index::reach(int src,int trg, ExceptionList* el){
//	if(src == trg){
//		return true;
//	}
//
//	if(!contains(src,trg))						// if it
//does not contain reject 		return false;
//
//	if(el!=NULL){
//// if using exception lists 			if(el->isAnException(src,trg))	// if it is an
//exception, reject 				return false; 			else 				return true;
//	}
//	visited[src]=++QueryCnt;
//	return go_for_reach(src,trg);
// }

bool Index::reach_lf(int src, int trg, ExceptionList *el) {
  if (src == trg)
    return true;
  if (!contains(src, trg)) // if it does not contain reject
    return false;

  if (g[src].top_level >= g[trg].top_level) // if using level filter, reject if
                                            // in a higher topological level
    return false;

  if (el != NULL) {                  // if using exception lists
    if (el->isAnException(src, trg)) // if it is an exception, reject
      return false;
    else
      return true;
  }

  visited[src] = ++QueryCnt;

  return go_for_reach_lf(src, trg);
}

bool Index::reachPP(int src, int trg, ExceptionList *el) {

  if (src == trg) {
    return true;
  }

  int res = containsPP(src, trg);
  if (res) { // if it does not contain reject
    switch (res) {
    case -1:
#ifdef DEBUG
      NegativeCut++;
#endif
      return false;
    case 1:
#ifdef DEBUG
      PositiveCut++;
      TotalDepth++;
#endif
      return true;
    }
  }
  CurrentDepth = 0;
  if (el != NULL) {                  // if using exception lists
    if (el->isAnException(src, trg)) // if it is an exception, reject
      return false;
    else
      return true;
  }
  visited[src] = ++QueryCnt;
  return go_for_reachPP(src, trg);
}

bool Index::reachPP_lf(int src, int trg, ExceptionList *el) {

  if (src == trg) {
    return true;
  }

  if (g[src].top_level >= g[trg].top_level) // if using level filter, reject if
                                            // in a higher topological level
    return false;
  int res = containsPP(src, trg);
  if (res) { // if it does not contain reject
    switch (res) {
    case -1:
#ifdef DEBUG
      NegativeCut++;
#endif
      return false;
    case 1:
#ifdef DEBUG
      PositiveCut++;
      TotalDepth++;
#endif
      return true;
    }
  }
  CurrentDepth = 0;
  if (el != NULL) {                  // if using exception lists
    if (el->isAnException(src, trg)) // if it is an exception, reject
      return false;
    else
      return true;
  }
  visited[src] = ++QueryCnt;
  return go_for_reachPP_lf(src, trg);
}

} // namespace lotus::cfl::cs_index::flare::grail
