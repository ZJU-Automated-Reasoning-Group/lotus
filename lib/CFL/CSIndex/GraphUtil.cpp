#include "CFL/CSIndex/GraphUtil.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <random>

// depth first search given a start node
void GraphUtil::dfs(Graph &g, int vid, vector<int> &preorder,
                    vector<int> &postorder, vector<bool> &visited) {
  visited[vid] = true;
  preorder.push_back(vid);
  EdgeList el = g.out_edges(vid);
  EdgeList::iterator eit;
  int nextid = -1;
  // check whether all child nodes are visited
  for (eit = el.begin(); eit != el.end(); eit++) {
    if (!visited[*eit]) {
      nextid = *eit;
      dfs(g, nextid, preorder, postorder, visited);
    }
  }
  postorder.push_back(vid);
}

// topological sorting by depth-first search
// return reverse order of topological_sorting list
void GraphUtil::topological_sort(Graph &g, vector<int> &ts) {
  vector<bool> visited(g.num_vertices(), false);
  vector<int> preorder;
  vector<int> postorder;
  vector<int> roots = g.getRoots();
  vector<int>::iterator sit;
  // depth-first-search whole graph
  for (sit = roots.begin(); sit != roots.end(); sit++)
    if (!visited[*sit])
      dfs(g, (*sit), preorder, postorder, visited);

  ts = postorder;
}

// compute transitive closure
// from Klaus Simon's paper "An improved algorithm for transitive closure on
// acyclic digraphs"
void GraphUtil::transitive_closure(Graph g, Graph &tc) {
  vector<int> ts;
  vector<int>::iterator vit;
  topological_sort(g, ts);

  EdgeList el;
  EdgeList::iterator eit;
  map<int, set<int>> ms;
  map<int, set<int>>::iterator mit;
  set<int>::iterator sit;
  // build set map iteratively
  // using reverse order of topological_sort list
  for (vit = ts.begin(); vit != ts.end(); vit++) {
    set<int> s;
    s.insert(*vit);
    ms[*vit] = s;
    el = g.out_edges(*vit);
    for (eit = el.begin(); eit != el.end(); eit++) {
      ms[*vit].insert(ms[*eit].begin(), ms[*eit].end());
    }
  }

  // add edges based on edge map
  for (mit = ms.begin(); mit != ms.end(); mit++) {
    set<int> s = (*mit).second;
    int src = (*mit).first;
    for (sit = s.begin(); sit != s.end(); sit++) {
      tc.addEdge(src, (*sit));
    }
  }
}

void GraphUtil::compute_pred(Graph g, vector<set<int>> &predMap) {
  vector<int> ts;
  topological_sort(g, ts);

  vector<int>::reverse_iterator rvit;
  EdgeList el;
  EdgeList::iterator eit;
  for (rvit = ts.rbegin(); rvit != ts.rend(); rvit++) {
    el = g.in_edges(*rvit);
    for (eit = el.begin(); eit != el.end(); eit++) {
      predMap[*rvit].insert(predMap[*eit].begin(), predMap[*eit].end());
    }
    predMap[*rvit].insert(*rvit);
  }
}

void GraphUtil::findTreeCover(Graph g, Graph &tree, vector<set<int>> &pred) {
  vector<int> ts;
  topological_sort(g, ts);
  findTreeCover(g, tree, pred, ts);
}

void GraphUtil::findTreeCover(Graph &g, Graph &tree, vector<set<int>> &pred,
                              vector<int> &ts) {
  vector<int>::reverse_iterator rvit;
  EdgeList el;
  EdgeList::iterator eit;
  int max_pred_size = -1;
  int max_pred;
  int pred_size;
  long sum = 0;
  for (rvit = ts.rbegin(); rvit != ts.rend(); rvit++) {
    el = g.in_edges(*rvit);
    max_pred_size = -1;
    max_pred = -1;
    for (eit = el.begin(); eit != el.end(); eit++) {
      pred_size = pred[*eit].size();
      if (pred_size > max_pred_size) {
        max_pred_size = pred[*eit].size();
        max_pred = *eit;
      }
      pred[*rvit].insert(pred[*eit].begin(), pred[*eit].end());
    }
    pred[*rvit].insert(*rvit);
    if (max_pred != -1) {
      tree.addEdge(max_pred, *rvit);
    }
    /*
    if (pred[*rvit].size() > g.num_vertices()) { cout << "error" << ''\n';
    exit(0); } sum += pred[*rvit].size(); cout << sum << "\t";
    */
  }
  //	cout << '\n';
}

// finding optimum tree-over by Rakesh Agrawal's paper
void GraphUtil::findTreeCover(Graph g, Graph &tree) {
  vector<int> ts;
  topological_sort(g, ts);

  vector<int>::reverse_iterator rvit;
  vector<set<int>> pred(g.num_vertices(), set<int>());
  EdgeList el;
  EdgeList::iterator eit;
  int max_pred_size = -1;
  int max_pred;
  int pred_size;
  for (rvit = ts.rbegin(); rvit != ts.rend(); rvit++) {
    el = g.in_edges(*rvit);
    max_pred_size = -1;
    max_pred = -1;
    for (eit = el.begin(); eit != el.end(); eit++) {
      pred_size = pred[*eit].size();
      if (pred_size > max_pred_size) {
        max_pred_size = pred[*eit].size();
        max_pred = *eit;
      }
      pred[*rvit].insert(pred[*eit].begin(), pred[*eit].end());
    }
    pred[*rvit].insert(*rvit);
    if (max_pred != -1) {
      tree.addEdge(max_pred, *rvit);
    } else
      tree.addVertex(*rvit);
  }
}

// find longest path
void GraphUtil::findTreeCoverL(Graph g, Graph &tree) {
  vector<int> ts;
  topological_sort(g, ts);

  vector<int>::reverse_iterator rvit;
  map<int, vector<int>> pred;
  EdgeList el;
  EdgeList::iterator eit;
  int max_pred_size = -1;
  int max_pred;
  int pred_size;
  for (rvit = ts.rbegin(); rvit != ts.rend(); rvit++) {
    el = g.in_edges(*rvit);
    max_pred_size = -1;
    max_pred = -1;
    for (eit = el.begin(); eit != el.end(); eit++) {
      pred_size = pred[*eit].size();
      if (pred_size > max_pred_size) {
        max_pred_size = pred[*eit].size();
        max_pred = *eit;
      }
    }
    if (max_pred != -1) {
      tree.addEdge(max_pred, *rvit);
      pred[*rvit] = pred[max_pred];
      pred[*rvit].push_back(max_pred);
    } else
      tree.addVertex(*rvit);
  }
}

// implement tarjan's algorithm to find Strongly Connected Component from a
// given start node
void GraphUtil::tarjan(Graph &g, int vid, int &index,
                       unordered_map<int, pair<int, int>> &order,
                       vector<int> &sn, multimap<int, int> &sccmap, int &scc) {
  order[vid].first = index;
  order[vid].second = index;
  index++;
  sn.push_back(vid);
  g[vid].visited = true;
  EdgeList el = g.out_edges(vid);
  EdgeList::iterator eit;
  for (eit = el.begin(); eit != el.end(); eit++) {
    if (!g[*eit].visited) {
      tarjan(g, *eit, index, order, sn, sccmap, scc);
      order[vid].second = min(order[*eit].second, order[vid].second);
    } else if (find(sn.begin(), sn.end(), (*eit)) != sn.end()) {
      order[vid].second = min(order[*eit].first, order[vid].second);
    }
  }

  if (order[vid].first == order[vid].second) {
    vector<int>::reverse_iterator rit;
    for (rit = sn.rbegin(); rit != sn.rend(); rit++) {
      if ((*rit) != vid) {
        sccmap.insert(make_pair(scc, *rit));
        //	sccmap[*rit] = scc;
        sn.pop_back();
      } else {
        sccmap.insert(make_pair(scc, *rit));
        //	sccmap[*rit] = scc;
        sn.pop_back();
        break;
      }
    }
    scc++;
  }
}

// merge Strongly Connected Component
// return vertex map between old vertex and corresponding new merged vertex
void GraphUtil::mergeSCC(Graph &g, int *on, vector<int> &reverse_topo_sort) {
  vector<int> sn;
  unordered_map<int, pair<int, int>> order;
  int ind = 0;
  multimap<int, int> sccmap; // each vertex id correspond with a scc num
  int scc = 0;
  int vid;
  int origsize = g.num_vertices();
  //	cout << " inside MergeSCC "<< '\n';
  for (int i = 0; i < origsize; i++) {
    vid = i;
    if (g[vid].visited)
      continue;
    tarjan(g, vid, ind, order, sn, sccmap, scc);
  }
  //	cout << " inside MergeSCC after tarjan "<< '\n';
  // no component need to merge
  if (scc == origsize) {
    for (int i = 0; i < origsize; i++)
      on[i] = i;
    // topological sort
    topological_sort(g, reverse_topo_sort);
    // update graph's topological id
    for (int i = 0; i < reverse_topo_sort.size(); i++)
      g[reverse_topo_sort[i]].topo_id = reverse_topo_sort.size() - i - 1;

    return;
  }

  unordered_map<int, vector<int>> inlist, outlist;
  g.extract(inlist, outlist);
  multimap<int, int>::iterator mit;
  mit = sccmap.begin();
  int num_comp;
  int maxid = g.num_vertices() - 1;
  while (mit != sccmap.end()) {
    num_comp = mit->first;

    if (++sccmap.lower_bound(num_comp) == sccmap.upper_bound(num_comp)) {
      on[mit->second] = mit->second;
      ++mit;
      continue;
    }

    maxid++;
    inlist[maxid] = vector<int>();
    outlist[maxid] = vector<int>();

    for (; mit != sccmap.upper_bound(num_comp); ++mit) {
      on[mit->second] = maxid;

      vector<int> vec = inlist[mit->second];
      vector<int>::iterator vit, vit1;
      vector<int> vec1;
      bool hasEdge = false;

      // copy all incoming edges
      for (vit = vec.begin(); vit != vec.end(); vit++) {
        hasEdge = false;
        vec1 = outlist[*vit];
        for (vit1 = vec1.begin(); vit1 != vec1.end(); vit1++) {
          if (*vit1 == maxid) {
            hasEdge = true;
            break;
          }
        }
        if (!hasEdge && (*vit != maxid)) {
          inlist[maxid].push_back(*vit);
          outlist[*vit].push_back(maxid);
        }
      }

      // copy all outgoing edges
      vec = outlist[mit->second];
      for (vit = vec.begin(); vit != vec.end(); vit++) {
        hasEdge = false;
        vec1 = inlist[*vit];
        for (vit1 = vec1.begin(); vit1 != vec1.end(); vit1++)
          if (*vit1 == maxid) {
            hasEdge = true;
            break;
          }
        if (!hasEdge && (*vit != maxid)) {
          outlist[maxid].push_back(*vit);
          inlist[*vit].push_back(maxid);
        }
      }

      // delete old vertex
      vec = inlist[mit->second];
      for (vit = vec.begin(); vit != vec.end(); vit++) {
        for (vit1 = outlist[*vit].begin(); vit1 != outlist[*vit].end();)
          if (*vit1 == mit->second)
            outlist[*vit].erase(vit1);
          else
            vit1++;
      }
      vec = outlist[mit->second];
      for (vit = vec.begin(); vit != vec.end(); vit++) {
        for (vit1 = inlist[*vit].begin(); vit1 != inlist[*vit].end();)
          if (*vit1 == mit->second)
            inlist[*vit].erase(vit1);
          else
            vit1++;
      }
      outlist.erase(mit->second);
      inlist.erase(mit->second);
    }
  }

  g = Graph(inlist, outlist);

  // topological sort
  topological_sort(g, reverse_topo_sort);
  // update graph's topological id
  for (int i = 0; i < reverse_topo_sort.size(); i++)
    g[reverse_topo_sort[i]].topo_id = reverse_topo_sort.size() - i - 1;

  // update index map
  unordered_map<int, int> indexmap;
  unordered_map<int, vector<int>>::iterator hit;
  int k;
  for (hit = outlist.begin(), k = 0; hit != outlist.end(); hit++, k++) {
    indexmap[hit->first] = k;
  }
  for (k = 0; k < origsize; k++)
    on[k] = indexmap[on[k]];
}

void GraphUtil::topo_leveler(Graph &g) {
  int N = g.num_vertices();
  vector<int>::iterator sit;
  // depth-first-search whole graph
  for (int i = 0; i < N; i++)
    topo_level(g, i);
}

int GraphUtil::topo_level(Graph &g, int vid) {
  if (g[vid].top_level != -1) {
    return g[vid].top_level;
  }
  int min = g.num_vertices();
  int max = -1;
  g[vid].top_level = 0;
  EdgeList el = g.in_edges(vid);
  EdgeList::iterator eit;
  for (eit = el.begin(); eit != el.end(); eit++) {
    max = max > topo_level(g, *eit) ? max : g[*eit].top_level;
    min = min < g[*eit].top_level ? min : g[*eit].top_level;
  }
  g[vid].top_level = max + 1;
  g[vid].min_parent_level = (min == g.num_vertices() ? -1 : min);
  return g[vid].top_level;
}

// traverse tree to label node with pre and post order by giving a start node
// using GRIPP's labeling method
void GraphUtil::traverse(Graph &tree, int vid, int &pre_post,
                         vector<bool> &visited) {
  visited[vid] = true;
  EdgeList el = tree.out_edges(vid);
  EdgeList::iterator eit;
  int pre_order;
  for (eit = el.begin(); eit != el.end(); eit++) {
    pre_order = pre_post;
    pre_post++;
    if (!visited[*eit])
      traverse(tree, *eit, pre_post, visited);
    tree[*eit].pre_order = pre_order;
    tree[*eit].post_order = pre_post;
    pre_post++;
  }
}

// compute interval label for each node of tree (pre_order, post_order)
void GraphUtil::pre_post_labeling(Graph &tree) {
  vector<int> roots = tree.getRoots();
  vector<int>::iterator sit;
  int pre_post = 0;
  int pre_order = 0;
  vector<bool> visited(tree.num_vertices(), false);

  for (sit = roots.begin(); sit != roots.end(); sit++) {
    pre_order = pre_post;
    pre_post++;
    traverse(tree, *sit, pre_post, visited);
    tree[*sit].pre_order = pre_order;
    tree[*sit].post_order = pre_post;
    pre_post++;
  }

  // for test
  /*
          cout << "Labeling spanning tree" << '\n';
          VertexList vl = tree.vertices();
          VertexList::iterator vit;
          int i = 0;
          for (vit = vl.begin(),i=0; vit != vl.end(); vit++,i++)
                  cout << i << ":\t" << tree[i].pre_order << "," <<
     tree[i].post_order << '\n';
  */
}

// void GraphUtil::treePathDecomposition(Graph tree, Graph& g,
// unordered_map<int, list<int> >& pathMap) {
void GraphUtil::treePathDecomposition(Graph tree, Graph &g,
                                      vector<vector<int>> &pathMap) {
  vector<int> roots = tree.getRoots();
  deque<int> que(roots.begin(), roots.end());

  int nextid, path_num = 0;
  EdgeList el;
  EdgeList::iterator eit;
  bool getone = false;
  vector<bool> visited(tree.num_vertices(), false);
  vector<int> path;
  while (!que.empty()) {
    nextid = que.front();
    que.pop_front();
    if (visited[nextid])
      continue;
    el = tree.out_edges(nextid);
    visited[nextid] = true;
    path.push_back(nextid);
    g[nextid].path_id = path_num;
    if (el.size() == 0) {
      pathMap.push_back(path);
      path = vector<int>();
      path_num++;
      continue;
    }
    do {
      getone = false;
      for (eit = el.begin(); eit != el.end(); eit++) {
        if (!visited[*eit] && !getone) {
          nextid = *eit;
          path.push_back(nextid);
          g[nextid].path_id = path_num;
          visited[nextid] = true;
          getone = true;
        } else if (!visited[*eit] && getone)
          que.push_back(*eit);
      }
      if (!getone) {
        pathMap.push_back(path);
        path = vector<int>();
        break;
      }
      el = tree.out_edges(nextid);
    } while (el.size() > 0);
    if (el.size() == 0) {
      pathMap.push_back(path);
      path = vector<int>();
    }
    path_num++;
  }

  // for test
  /*
          cout << "tree path decomposition " << '\n';
          vector<vector<int> >::iterator mlit;
          vector<int> path1;
          vector<int>::iterator lit;
          int i = 0;
          for (mlit = pathMap.begin(), i=0; mlit != pathMap.end(); mlit++, i++)
     { path1 = *mlit; cout << "path [" << i << "]: "; for (lit = path1.begin();
     lit != path1.end(); lit++) cout << *lit << " "; cout << '\n';
          }
  */
}

// path decomposition
void GraphUtil::pathDecomposition(Graph &g, vector<vector<int>> &pathMap,
                                  vector<int> reverse_topo_sort) {
  vector<int>::reverse_iterator rit;
  EdgeList el;
  EdgeList::iterator eit;
  int i = 0, k;
  int min;
  int min_id;
  vector<bool> visited(g.num_vertices(), false);
  vector<int> path;

  for (rit = reverse_topo_sort.rbegin(), i = 0; rit != reverse_topo_sort.rend();
       rit++) {
    k = *rit;
    if (visited[k])
      continue;
    visited[k] = true;
    path.push_back(k);
    g[k].path_id = i;
    do {
      min = MAX_VAL;
      min_id = -1;
      el = g.out_edges(k);
      for (eit = el.begin(); eit != el.end(); eit++) {
        if (!visited[*eit] && g[*eit].topo_id < min) {
          min = g[*eit].topo_id;
          min_id = *eit;
        }
      }
      if (min_id != -1) {
        visited[min_id] = true;
        g[min_id].path_id = i;
        path.push_back(min_id);
        k = min_id;
      }
    } while (min_id != -1);
    pathMap.push_back(path);
    path = vector<int>();
    i++;
  }
  // for test
  /*
          cout << "path  decomposition " << '\n';
          unordered_map<int, list<int> >::iterator mlit;
          list<int> path;
          list<int>::iterator lit;
          for (mlit = pathMap.begin(); mlit != pathMap.end(); mlit++) {
                  path = mlit->second;
                  cout << "path [" << mlit->first << "]: ";
                  for (lit = path.begin(); lit != path.end(); lit++)
                          cout << *lit << " ";
                  cout << '\n';
          }
  */
}

// path decomposition
void GraphUtil::pathDecomposition(Graph &g, vector<vector<int>> &pathMap) {
  vector<int> reverse_topo_sort;
  // topological sort
  topological_sort(g, reverse_topo_sort);
  pathDecomposition(g, pathMap, reverse_topo_sort);
}

// for test
void GraphUtil::genRandomGraph(int n, double c, char *filename) {
  int threshold = int(c * 1.00 / (n * 1.00) * 10000);
  Graph g;
  int i, j;
  int rand_num;
  for (i = 0; i < n; i++)
    g.addVertex(i);

  srand(time(NULL));
  for (i = 0; i < n; i++) {
    for (j = 0; j < n; j++)
      if (i != j) {
        rand_num = rand() % 10000;
        if (rand_num < threshold)
          g.addEdge(i, j);
      }
  }

  ofstream out(filename);
  g.writeGraph(out);
}

// pure DFS as benchmark to verify other queries
bool GraphUtil::DFSCheck(Graph &graph, int vid, bit_vector *visited, int trg) {
#ifdef DFSDEBUG
  cout << graph[vid].o_vid << "[" << vid << "]" << '\n';
#endif
  if (vid == trg)
    return true;
  visited->set_one(vid);
  EdgeList el = graph.out_edges(vid);
  EdgeList::iterator eit;
  bool result = false;
  for (eit = el.begin(); eit != el.end(); eit++) {
    if (!visited->get(*eit)) {
      result = DFSCheck(graph, *eit, visited, trg);
      if (result)
        return true;
    }
  }
  return false;
}

bool GraphUtil::DFSReach(Graph &graph, int src, int trg) {
#ifdef DFSDEBUG
  cout << "DFS path: " << "(" << src << ", " << trg << "[" << graph[trg].o_vid
       << "])" << '\n';
#endif
  int gsize = graph.num_vertices();
  bit_vector *visited = new bit_vector(gsize);
  bool result = DFSCheck(graph, src, visited, trg);
  delete visited;
#ifdef DFSDEBUG
  cout << '\n';
#endif
  return result;
}

void GraphUtil::buildGateGraphByNodeFast(Graph &g, bit_vector *isgates, int vid,
                                         int radius, map<int, int> &gateindex,
                                         Graph &gategraph, vector<int> &que,
                                         vector<int> &dist, int &ref) {
  int u;
  EdgeList el;
  EdgeList::iterator eit;
  int gateid = gateindex[vid];
  ref += radius + 2;
  int index = 0, endindex = 1, val, nid;
  que[index] = vid;
  dist[vid] = ref;
  while (index < endindex) {
    u = que[index];
    index++;
    val = dist[u];
    el = g.out_edges(u);
    for (eit = el.begin(); eit != el.end(); eit++) {
      nid = (*eit);
      if (dist[nid] < ref) {
        dist[nid] = val + 1;
        if (isgates->get(nid)) {
          gategraph.addEdge(gateid, gateindex[nid]);
          continue;
        }
        if (val + 1 - ref < radius) {
          que[endindex++] = nid;
        }
      }
    }
  }
  /*
  while (!que.empty()) {
          u = que.front();
          que.pop_front();
          if (isgates->get(u) && u!=vid) {
                  gategraph.addEdge(gateindex[vid],gateindex[u]);
          }
          if (dist[u]==radius) continue;
          el = g.out_edges(u);
          for (eit = el.begin(); eit != el.end(); eit++) {
                  if (dist[*eit]==0) {
                          dist[*eit] = dist[u]+1;
                          if (dist[*eit]<=radius)
                                  que.push_back(*eit);
                  }
          }
  }
  */
  //	gategraph.sortEdges();
}

int GraphUtil::buildGateGraphByNodeFastWrite(Graph &g, bit_vector *isgates,
                                             int vid, int radius,
                                             map<int, int> &gateindex,
                                             ostream &out, vector<int> &que,
                                             vector<int> &dist, int &ref) {
  int u;
  EdgeList el;
  EdgeList::iterator eit;
  int gateid = gateindex[vid];
  vector<int> neighbors;
  ref += radius + 2;
  int index = 0, endindex = 1, val, nid;
  que[index] = vid;
  dist[vid] = ref;
  while (index < endindex) {
    u = que[index];
    index++;
    val = dist[u];
    el = g.out_edges(u);
    for (eit = el.begin(); eit != el.end(); eit++) {
      nid = (*eit);
      if (dist[nid] < ref) {
        dist[nid] = val + 1;
        if (isgates->get(nid)) {
          //		gategraph.addEdge(gateid,gateindex[nid]);
          neighbors.push_back(gateindex[nid]);
          continue;
        }
        if (val + 1 - ref < radius) {
          que[endindex++] = nid;
        }
      }
    }
  }
  //	gategraph.sortEdges();
  out << gateid << ": ";
  sort(neighbors.begin(), neighbors.end());
  for (int i = 0; i < neighbors.size(); i++) {
    out << neighbors[i] << " ";
  }
  out << "#" << '\n';
  out.flush();
  return neighbors.size();
}

int GraphUtil::buildGateGraphWrite(Graph &graph, bit_vector *isgate, int radius,
                                   ostream &out) {
  int gsize = graph.num_vertices();
  map<int, int> gateindex;
  int index = 0;
  for (int i = 0; i < gsize; i++) {
    if (isgate->get(i)) {
      gateindex[i] = index;
      index++;
    }
  }
  map<int, int>::iterator mit;
  vector<int> que = vector<int>(gsize, 0);
  vector<int> dist = vector<int>(gsize, 0);
  int ref = 0;
  int edgesize = 0;
  out << "graph_for_greach" << '\n';
  out << index << '\n';
  for (mit = gateindex.begin(); mit != gateindex.end(); mit++) {
    int esize = buildGateGraphByNodeFastWrite(graph, isgate, mit->first, radius,
                                              gateindex, out, que, dist, ref);
    edgesize += esize;
  }
  return edgesize;
}

bool GraphUtil::DFSCheckCnt(Graph &graph, int vid, vector<int> &visited,
                            int trg, int qcnt) {
  if (vid == trg)
    return true;
  visited[vid] = qcnt;
  EdgeList el = graph.out_edges(vid);
  EdgeList::iterator eit;
  bool result = false;
  for (eit = el.begin(); eit != el.end(); eit++) {
    if (visited[*eit] != qcnt) {
      if (DFSCheckCnt(graph, *eit, visited, trg, qcnt))
        return true;
    }
  }
  return false;
}

bool GraphUtil::DFSReachCnt(Graph &graph, int src, int trg,
                            vector<int> &visited, int &qcnt) {
  qcnt++;
  return DFSCheckCnt(graph, src, visited, trg, qcnt);
}

void GraphUtil::collectInLocalGates(Graph &g, const bit_vector *isgates,
                                    int vid, int radius, vector<int> &ingates) {
  int u;
  EdgeList el;
  EdgeList::iterator eit;
  deque<int> que;
  map<int, int> dist;
  que.push_back(vid);
  dist[vid] = 0;
  ingates.clear();
  while (!que.empty()) {
    u = que.front();
    que.pop_front();
    if (isgates->get(u) && u != vid) {
      ingates.push_back(u);
      continue;
    }
    if (dist[u] == radius)
      continue;
    el = g.in_edges(u);
    for (eit = el.begin(); eit != el.end(); eit++) {
      if (dist[*eit] == 0) {
        dist[*eit] = dist[u] + 1;
        if (dist[*eit] <= radius)
          que.push_back(*eit);
      }
    }
  }
  if (isgates->get(vid)) {
    ingates.clear();
    ingates.push_back(vid);
  }
  //	sort(ingates.begin(),ingates.end());
}

void GraphUtil::collectOutLocalGates(Graph &g, const bit_vector *isgates,
                                     int vid, int radius,
                                     vector<int> &outgates) {
  int u;
  EdgeList el;
  EdgeList::iterator eit;
  deque<int> que;
  map<int, int> dist;
  que.push_back(vid);
  dist[vid] = 0;
  outgates.clear();
  while (!que.empty()) {
    u = que.front();
    que.pop_front();
    if (isgates->get(u) && u != vid)
      outgates.push_back(u);
    if (dist[u] == radius)
      continue;
    el = g.out_edges(u);
    for (eit = el.begin(); eit != el.end(); eit++) {
      if (dist[*eit] == 0) {
        dist[*eit] = dist[u] + 1;
        if (dist[*eit] <= radius)
          que.push_back(*eit);
      }
    }
  }
  //	sort(outgates.begin(),outgates.end());
}

bool GraphUtil::BFSOutLocalReach(Graph &g, const bit_vector *isgates,
                                 int radius, int src, int trg,
                                 vector<int> &outgates) {
  int u;
  EdgeList el;
  EdgeList::iterator eit;
  deque<int> que;
  map<int, int> dist;
  que.push_back(src);
  dist[src] = 0;
  outgates.clear();
  while (!que.empty()) {
    u = que.front();
    if (u == trg)
      return true;
    que.pop_front();
    if (isgates->get(u) && u != src)
      outgates.push_back(u);
    if (dist[u] == radius)
      continue;
    el = g.out_edges(u);
    for (eit = el.begin(); eit != el.end(); eit++) {
      if (dist[*eit] == 0) {
        dist[*eit] = dist[u] + 1;
        if (dist[*eit] <= radius)
          que.push_back(*eit);
      }
    }
  }
  if (isgates->get(src)) {
    outgates.clear();
    outgates.push_back(src);
  }
  return false;
}

int GraphUtil::visit(Graph &tree, int vid, int &pre_post, vector<bool> &visited,
                     vector<pair<int, int>> &dfslabels) {
  visited[vid] = true;
  EdgeList el = tree.out_edges(vid);
  std::random_device rd;
  std::mt19937 g(rd());
  std::shuffle(el.begin(), el.end(), g);
  EdgeList::iterator eit;
  int pre_order = tree.num_vertices() + 1;
  for (eit = el.begin(); eit != el.end(); eit++) {
    if (!visited[*eit]) {
      pre_order =
          min(pre_order, visit(tree, *eit, pre_post, visited, dfslabels));
    } else
      pre_order = min(pre_order, dfslabels[*eit].first);
    //	pre_order=min(pre_order,tree[*eit].pre->back());
    // pre_order=min(pre_order,tree[*eit].pre->at(tree[*eit].pre->size()-1));
  }

  pre_order = min(pre_order, pre_post);
  //	tree[vid].pre->push_back(pre_order);
  //	tree[vid].post->push_back(pre_post);
  dfslabels[vid].first = pre_order;
  dfslabels[vid].second = pre_post;
  pre_post++;
  return pre_order;
}

void GraphUtil::grail_labeling(Graph &tree, vector<pair<int, int>> &dfslabels) {
  vector<int> roots = tree.getRoots();
  vector<int>::iterator sit;
  int pre_post = 0;
  vector<bool> visited(tree.num_vertices(), false);
  dfslabels.clear();
  dfslabels =
      vector<pair<int, int>>(tree.num_vertices(), pair<int, int>(-1, -1));
  std::random_device rd;
  std::mt19937 g(rd());
  std::shuffle(roots.begin(), roots.end(), g);
  for (sit = roots.begin(); sit != roots.end(); sit++) {
    pre_post++;
    visit(tree, *sit, pre_post, visited, dfslabels);
  }
}
