#include "CFL/CSIndex/PathTree.h"

#include <algorithm>

/**
 * @brief Constructor with graph reference.
 * @param graph Reference to the main graph
 *
 * Initializes a path tree with the given graph and allocates necessary data
 * structures.
 */
PathTree::PathTree(Graph &graph) : g(graph) {
  int maxid = g.num_vertices();
  labels = new int *[maxid];
  for (int i = 0; i < maxid; i++)
    labels[i] = new int[3];
  ng = Graph(maxid);
  nextVertex = vector<int>(maxid);
  //	out_uncover = vector<set<int> >(maxid);
  out_uncover = vector<vector<int>>(maxid);
  for (int i = 0; i < maxid; i++) {
    ng.addVertex(i);
    nextVertex[i] = -1;
    //		out_uncover[i] = set<int>();
    out_uncover[i] = vector<int>();
  }
}

/**
 * @brief Constructor with graph reference and topological sort.
 * @param graph Reference to the main graph
 * @param ts Topological sort vector
 *
 * Initializes a path tree with the given graph and topological sort
 * information.
 */
PathTree::PathTree(Graph &graph, vector<int> ts) : g(graph) {
  grts = ts;
  int maxid = g.num_vertices();
  cout << "debug: maxid " << maxid << '\n';
  labels = new int *[maxid];
  for (int i = 0; i < maxid; i++)
    labels[i] = new int[3];
  ng = Graph(maxid);
  nextVertex = vector<int>(maxid);
  //	out_uncover = vector<set<int> >(maxid);
  out_uncover = vector<vector<int>>(maxid);
  for (int i = 0; i < maxid; i++) {
    ng.addVertex(i);
    nextVertex[i] = -1;
    //		out_uncover[i] = set<int>();
    out_uncover[i] = vector<int>();
  }
}

/**
 * @brief Destructor.
 *
 * Cleans up dynamically allocated memory for labels.
 */
PathTree::~PathTree() {
  for (int i = 0; i < g.num_vertices(); i++)
    delete[] labels[i];
  delete[] labels;
}

void PathTree::compute_tcm() {
  double tcsize = 0;
  Graph tc(g.num_vertices());
  GraphUtil::transitive_closure(g, tc);
  EdgeList el;
  EdgeList::iterator eit;
  for (int i = 0; i < tc.num_vertices(); i++) {
    el = tc.out_edges(i);
    tcsize += el.size();
    for (eit = el.begin(); eit != el.end(); eit++)
      tcm[make_pair(i, *eit)] = true;
  }
  cout << "#TC size=" << tcsize << '\n';
  cout << "#Ratio=" << tcsize / (g.num_vertices() * 1.0) << '\n';
  // for test
  /*
          cout << "=======================TC===================" << '\n';
          tc.printGraph();
  */
}

void PathTree::index_size(int *ind_size) {
  int isize = 0;
  int uncover_size = 0;
  //	displayLabels();
  vector<vector<int>>::iterator vit;
  for (vit = out_uncover.begin(); vit != out_uncover.end(); vit++) {
    uncover_size += vit->size();
  }
  cout << "uncover_size=" << uncover_size << '\n';
  isize += uncover_size;
  isize += g.num_vertices();
  isize += pathMap.size() * 2;

  int uncover_size_tab = uncover_size;
  map<int, vector<int>>::iterator mit;
  for (mit = comp_table.begin(); mit != comp_table.end(); mit++)
    uncover_size_tab += mit->second.size();
  /*
          cout << "uncover set rate: " << uncover_size*1.00/(isize*1.00) <<
     '\n'; cout << "path tree cover set size percentage: " <<
     pathMap.size()*2.00/(isize*1.00) << '\n';
  */
  ind_size[0] = isize;            // total size
  ind_size[1] = uncover_size_tab; // transitive closure size
  ind_size[2] = uncover_size;
  /*
          int psize = 0;
          for (int i = 0; i < pathMap.size(); i++)
                  psize += pathMap[i].size()-1;
          cout << "path cover edge size: " << psize << '\n';
  */
}

void PathTree::transform(DWGraph branch, Graph &graph) {
  DWVertexList dwvl = branch.vertices();
  DWVertexList::iterator dwvit;
  DWEdgeList dwel;
  DWEdgeList::iterator dweit;
  for (int i = 0; i < branch.num_vertices(); i++)
    graph.addVertex(i);
  for (dwvit = dwvl.begin(); dwvit != dwvl.end(); dwvit++) {
    dwel = branch.out_edges(dwvit->first);
    for (dweit = dwel.begin(); dweit != dwel.end(); dweit++) {
      graph.addEdge(dwvit->first, branch.target(*dweit));
    }
  }
  /*
          cout << "------------------------------------" <<'\n';
          graph.printGraph();
  */
}

void PathTree::buildWeightPathGraph_Pred() {
  int gs = g.num_vertices();
  // perform path decomposition
  //	GraphUtil::pathDecomposition(g, pathMap);
  Graph tree(gs);
  for (int i = 0; i < gs; i++)
    tree.addVertex(i);
  cout << "complete tree init" << '\n';

  gettimeofday(&before_time, NULL);
  vector<set<int>> predMap(g.num_vertices(), set<int>());
  cout << "start find tree cover" << '\n';
  GraphUtil::findTreeCover(g, tree, predMap, grts);
  gettimeofday(&after_time, NULL);
  run_time = (after_time.tv_sec - before_time.tv_sec) * 1000.0 +
             (after_time.tv_usec - before_time.tv_usec) * 1.0 / 1000.0;
  cout << "find tree cover time:" << run_time << " (ms)" << '\n';

  gettimeofday(&before_time, NULL);

  GraphUtil::treePathDecomposition(tree, g, pathMap);

  gettimeofday(&after_time, NULL);
  run_time = (after_time.tv_sec - before_time.tv_sec) * 1000.0 +
             (after_time.tv_usec - before_time.tv_usec) * 1.0 / 1000.0;
  cout << "path decomposition time:" << run_time << " (ms)" << '\n';

  // build weight path graph
  unordered_map<int, int> fastMap;
  unordered_map<int, int> weightMap;
  unordered_map<int, int>::iterator hmit;
  vector<vector<int>>::iterator mit;
  vector<int> path;
  vector<int>::iterator lit;
  EdgeList el;
  EdgeList::iterator eit;
  vector<int> vec;
  vector<int>::iterator v_end;

  // construct equivalent topology
  Graph equgraph;
  map<int, set<int>> pathtopo;
  for (int i = 0; i < gs; i++) {
    equgraph.addVertex(i);
    el = g.out_edges(i);
    for (eit = el.begin(); eit != el.end(); eit++) {
      if (g[*eit].path_id != g[i].path_id)
        pathtopo[g[i].path_id].insert(g[*eit].path_id);
    }
  }
  buildEquEdgeset(pathtopo, equgraph);

  // build weighted path graph
  int edgeid = 0;

  int index, i, k, pre, addval;
  int gsize = pathMap.size() + 10;
  for (mit = pathMap.begin(), k = 0; mit != pathMap.end(); mit++, k++) {
    pg.addVertex(k);
    path = (*mit);
    for (lit = path.begin(), i = 0; lit != path.end(); lit++, i++) {
      el = equgraph.in_edges(*lit);
      if (i == 0) {
        for (eit = el.begin(); eit != el.end(); eit++) {
          if (g[*eit].path_id == k)
            continue;
          index = g[*eit].path_id * gsize + k;
          weightMap[index] += predMap[*eit].size();
          hmit = fastMap.find(index);
          if (hmit != fastMap.end()) {
            pg.addEdge(g[*eit].path_id, k, weightMap[index], hmit->second);
          } else {
            fastMap[index] = edgeid;
            pg.addEdge(g[*eit].path_id, k, weightMap[index], edgeid);
            if (edgeid == 475268) {
              cout << "found: " << edgeid << '\n';
            }
            edgeid++;
          }
        }
        pre = *lit;
      } else {
        for (eit = el.begin(); eit != el.end(); eit++) {
          index = g[*eit].path_id * gsize + k;
          if (g[*eit].path_id != k) {
            vec.clear();
            set_difference(predMap[*eit].begin(), predMap[*eit].end(),
                           predMap[pre].begin(), predMap[pre].end(),
                           back_inserter(vec));
            addval = vec.size();
            if (addval == 0)
              addval = 1;
            weightMap[index] += addval;
            hmit = fastMap.find(index);

            if (hmit != fastMap.end()) {
              pg.addEdge(g[*eit].path_id, k, weightMap[index], hmit->second);
            } else {
              fastMap[index] = edgeid;
              pg.addEdge(g[*eit].path_id, k, weightMap[index], edgeid);
              if (edgeid == 475268) {
                cout << "found: " << edgeid << '\n';
              }
              edgeid++;
            }
          }
        }
        pre = *lit;
      }
    }
  }
  maxeid = edgeid;
  cout << "debug: maxedi: " << maxeid << '\n';
}

// build weighted path graph
void PathTree::buildWeightPathGraph(int type) {
  // perform path decomposition
  if (type == 4)
    GraphUtil::pathDecomposition(g, pathMap, grts);

  // build weight path graph
  unordered_map<int, int> fastMap;
  unordered_map<int, int>::iterator hmit;
  vector<vector<int>>::iterator mit;
  vector<int> path;
  vector<int>::iterator lit;
  EdgeList el;
  EdgeList::iterator eit;
  int edgeid = 0;
  int depth, k = 0;
  int gsize = pathMap.size() + 10;

  for (mit = pathMap.begin(), k = 0; mit != pathMap.end(); mit++, k++) {
    pg.addVertex(k);
    depth = 1;
    path = (*mit);
    for (lit = path.begin(); lit != path.end(); lit++) {
      el = g.out_edges(*lit);
      for (eit = el.begin(); eit != el.end(); eit++) {
        if (g[*eit].path_id != k) {
          hmit = fastMap.find(k * gsize + g[*eit].path_id);
          if (hmit != fastMap.end())
            pg.addEdge(k, g[*eit].path_id, depth, hmit->second);
          //	pg.addEdge(mit->first, g[*eit].path_id, depth, hmit->second);
          else {
            //	fastMap[mit->first*gsize+g[*eit].path_id] = edgeid;
            fastMap[k * gsize + g[*eit].path_id] = edgeid;
            //	pg.addEdge(mit->first, g[*eit].path_id, depth, edgeid);
            pg.addEdge(k, g[*eit].path_id, depth, edgeid);
            edgeid++;
          }
        }
      }
      depth++;
    }
  }
  maxeid = edgeid;
}

void PathTree::buildEquEdgeset(map<int, set<int>> &pathtopo, Graph &equgraph) {
  // add all vertices from original graph
  vector<vector<int>>::iterator mit;
  vector<int> path;
  vector<int>::iterator lit;

  EdgeList el, el1;
  EdgeList::iterator eit, eit1;
  int source_path_maxtopo = 0;
  int max_id;
  int max_topo_id = MIN_VAL;
  int depth;
  map<int, set<int>>::iterator miter;
  set<int>::iterator siter;
  set<int> si;
  for (miter = pathtopo.begin(); miter != pathtopo.end(); miter++) {
    si = miter->second;
    for (siter = si.begin(); siter != si.end(); siter++) {
      path = pathMap[*siter];
      source_path_maxtopo = MIN_VAL;
      for (lit = path.begin(); lit != path.end(); lit++) {
        // find maximum topological id
        max_id = -1;
        max_topo_id = MIN_VAL;
        el1 = g.in_edges(*lit);
        for (eit1 = el1.begin(); eit1 != el1.end(); eit1++) {
          if (g[*eit1].path_id == miter->first &&
              g[*eit1].topo_id > max_topo_id) {
            max_id = *eit1;
            max_topo_id = g[*eit1].topo_id;
          }
        }
        if (max_id == -1 || max_topo_id <= source_path_maxtopo)
          continue;
        source_path_maxtopo = max_topo_id;
        equgraph.addEdge(max_id, *lit);
      }
    }
  }
}

// calculate minimal equivalent edgeset
void PathTree::buildEquGraph() {
  // add all vertices from original graph
  vector<vector<int>>::iterator mit;
  vector<int> path;
  vector<int>::iterator lit;
  int p, q;
  for (mit = pathMap.begin(); mit != pathMap.end(); mit++) {
    path = (*mit);
    if (path.size() == 1)
      continue;
    for (lit = path.begin(); lit != path.end();) {
      p = *lit;
      lit++;
      if (lit == path.end())
        break;
      q = *lit;
      nextVertex[p] = q;
      ng.addEdge(p, q);
    }
  }

  EdgeList el, el1;
  EdgeList::iterator eit, eit1;
  int source_path_maxtopo = 0;
  int max_id;
  int max_topo_id = MIN_VAL;
  int depth;
  int gsize = newbranch.num_vertices();
  for (int i = 0; i < gsize; i++) {
    el = newbranch.out_edges(i);
    for (eit = el.begin(); eit != el.end(); eit++) {
      path = pathMap[*eit];
      source_path_maxtopo = MIN_VAL;
      for (lit = path.begin(); lit != path.end(); lit++) {
        // find maximum topological id
        max_id = -1;
        max_topo_id = MIN_VAL;
        el1 = g.in_edges(*lit);
        for (eit1 = el1.begin(); eit1 != el1.end(); eit1++) {
          if (g[*eit1].path_id == i && g[*eit1].topo_id > max_topo_id) {
            max_id = *eit1;
            max_topo_id = g[*eit1].topo_id;
          }
        }
        if (max_id == -1 || max_topo_id <= source_path_maxtopo)
          continue;
        source_path_maxtopo = max_topo_id;
        ng.addEdge(max_id, *lit);
      }
    }
  }
}

void PathTree::pathDFS(int vid, int &order, int &first_order,
                       vector<bool> &visited) {
  visited[vid] = true;
  g[vid].first_visit = first_order;
  first_order++;
  if (nextVertex[vid] != -1) {
    if (!visited[nextVertex[vid]])
      pathDFS(nextVertex[vid], order, first_order, visited);
  }

  EdgeList el = ng.out_edges(vid);
  EdgeList::iterator eit;
  for (eit = el.begin(); eit != el.end(); eit++) {
    if (!visited[*eit])
      pathDFS(*eit, order, first_order, visited);
  }
  g[vid].dfs_order = order;
  order--;
}

void PathTree::readPathMap(ifstream &cfile) {
  int n;
  string buf;
  getline(cfile, buf);
  istringstream(buf) >> n;
  pathMap = vector<vector<int>>(n, vector<int>());
  string sub;
  int id;
  for (int i = 0; i < n; i++) {
    getline(cfile, buf);
    while (buf.find("]") != string::npos) {
      sub = buf.substr(1, buf.find("]"));
      istringstream(sub) >> id;
      pathMap[i].push_back(id);
      g[id].path_id = i;
      buf.erase(0, buf.find("]") + 1);
    }
  }
  cfile.close();

  // for test
  /*
  for (int i = 0; i < pathMap.size(); i++) {
          cout << "Path " << i << ": ";
          for (int j = 0; j < pathMap[i].size(); j++)
                  cout << pathMap[i][j] << ", ";
          cout << '\n';
  }
  */
}

// type specify PTree-1 or PTree-2
void PathTree::createLabels(int type, ifstream &cfile, bool compress) {
  struct timeval after_time1, before_time1;
  // build weighted path graph
  cout << "building weighted path graph" << '\n';
  gettimeofday(&before_time1, NULL);
  if (type == 1) {
    buildWeightPathGraph_Pred();
  } else if (type == 2 || type == 3) {
    readPathMap(cfile);
    buildWeightPathGraph(type);
  } else {
    buildWeightPathGraph(type);
  }
  gettimeofday(&after_time1, NULL);
  run_time = (after_time1.tv_sec - before_time1.tv_sec) * 1000.0 +
             (after_time1.tv_usec - before_time1.tv_usec) * 1.0 / 1000.0;
  cout << "building weighted path graph time:" << run_time << " (ms)" << '\n';

  /*
          pg.printGraph();
          cout << "path size " << pathMap.size() << '\n';
  */

  // Mar. 12nd by Ning
  // add virtual super node
  /*
  int maxid = pg.maxid();
  pg.addVertex(maxid+1);
  int eid = maxeid;
  DWVertexList::iterator dwvit;
  DWVertexList& dvlist = pg.vertices();
  for (dwvit = dvlist.begin(); dwvit != dvlist.end(); dwvit++, eid++)
          pg.addEdge(maxid+1, dwvit->first, 0, eid);
  */

  /*
          cout << "after add virtual super node" << '\n';
          pg.printGraph();
  */

  // find maximum branching
  cout << "finding max branching" << '\n';
  gettimeofday(&before_time, NULL);
  if (type == 1) {
#ifdef DEBUG
    pg.printGraph();
    DWGraphUtil::checkBranching(pg, branch);
    cout << "***********************************************" << '\n';
    branch.printGraph();
//	exit(0);
#else
    DWGraphUtil::findMaxBranching(pg, branch);
#endif
  } else {
#ifdef DEBUG
    DWGraphUtil::checkBranching1(pg, branch);
#else
    DWGraphUtil::findMaxBranching1(pg, branch);
#endif
  }

  gettimeofday(&after_time, NULL);
  run_time = (after_time.tv_sec - before_time.tv_sec) * 1000.0 +
             (after_time.tv_usec - before_time.tv_usec) * 1.0 / 1000.0;
  cout << "finding max branching time:" << run_time << " (ms)" << '\n';

  /*
          cout << "find max branching" << '\n';
          branch.printGraph();
  */

  // Mar. 12nd by Ning
  // remove virtual super node
  //	branch.removeVertex(maxid+1);

  /*
          cout << "after remove super node" << '\n';
          branch.printGraph();
  */

  // graph transform
  gettimeofday(&before_time, NULL);
  newbranch = Graph(branch.num_vertices());
  transform(branch, newbranch);

  gettimeofday(&after_time, NULL);
  run_time = (after_time.tv_sec - before_time.tv_sec) * 1000.0 +
             (after_time.tv_usec - before_time.tv_usec) * 1.0 / 1000.0;
  cout << "weighted graph transformation time:" << run_time << " (ms)" << '\n';
  /*
          cout << "++++++++++++++++++++++++++++++++++++++++++++" << '\n';
          newbranch.printGraph();
  */
  // calculate minimal equivalent EdgeSet
  cout << "calculating equivalent edgeset" << '\n';
  gettimeofday(&before_time, NULL);
  buildEquGraph();
  gettimeofday(&after_time, NULL);
  run_time = (after_time.tv_sec - before_time.tv_sec) * 1000.0 +
             (after_time.tv_usec - before_time.tv_usec) * 1.0 / 1000.0;
  cout << "building equivalent graph time:" << run_time << " (ms)" << '\n';
  /*
          cout << "equivalent graph" << '\n';
          ng.printGraph();
  */
  // labeling branch by GRIPP's algorithm
  cout << "labeling found max branching" << '\n';
  gettimeofday(&before_time, NULL);
  GraphUtil::pre_post_labeling(newbranch);

  gettimeofday(&after_time, NULL);
  run_time = (after_time.tv_sec - before_time.tv_sec) * 1000.0 +
             (after_time.tv_usec - before_time.tv_usec) * 1.0 / 1000.0;
  cout << "labeling branching time:" << run_time << " (ms)" << '\n';
  /*
          // for test
          cout << "pre post labeling" << '\n';
          newbranch.printGraph();
  */

  // create labels for every vertex
  cout << "labeling equivalent graph by depth-first-search" << '\n';
  vector<int> reverse_topo_sort;
  GraphUtil::topological_sort(newbranch, reverse_topo_sort);
  gettimeofday(&before_time, NULL);
  vector<int> path;
  vector<int>::iterator lit;
  vector<int>::iterator vit;
  vector<int>::reverse_iterator rit;
  int order = g.num_vertices();
  int first_order = 1;
  vector<bool> visited(order, false);
  for (rit = reverse_topo_sort.rbegin(); rit != reverse_topo_sort.rend();
       rit++) {
    path = pathMap[*rit];
    lit = path.begin();
    for (lit = path.begin(); lit != path.end(); lit++) {
      if (!visited[*lit])
        pathDFS(*lit, order, first_order, visited);
    }
  }

  gettimeofday(&after_time, NULL);
  run_time = (after_time.tv_sec - before_time.tv_sec) * 1000.0 +
             (after_time.tv_usec - before_time.tv_usec) * 1.0 / 1000.0;
  cout << "labeling equivalent graph by DFS time:" << run_time << " (ms)"
       << '\n';

  // update label vector
  int gsize = g.num_vertices();
  for (int i = 0; i < gsize; i++) {
    labels[i][0] = newbranch[g[i].path_id].pre_order;
    labels[i][1] = newbranch[g[i].path_id].post_order;
    labels[i][2] = g[i].dfs_order;
  }

  // handling edges not covered
  cout << "collecting path-tree uncovered vertices" << '\n';
  gettimeofday(&before_time, NULL);
  //	reverse_topo_sort = vector<int>();
  //	GraphUtil::topological_sort(g, reverse_topo_sort);
  EdgeList el;
  EdgeList::iterator eit;
  int pre1, post1, pre2, post2;
  for (vit = grts.begin(); vit != grts.end(); vit++) {
    el = g.out_edges(*vit);
    pre1 = labels[*vit][0];
    post1 = labels[*vit][1];
    // Nov 9 10 2010 for tods correction
    //		cout << "current vit: " << *vit << '\n';
    out_uncover[*vit].push_back(*vit);
    for (eit = el.begin(); eit != el.end(); eit++) {
      //	insertSet(out_uncover[*vit], out_uncover[*eit]);
      mergeVector(out_uncover[*vit], out_uncover[*eit]);
      if (labels[*vit][2] <= labels[*eit][2] && labels[*eit][0] >= pre1 &&
          labels[*eit][1] <= post1)
        continue;
      vector<int> temp;
      temp.push_back(*eit);
      mergeVector(out_uncover[*vit], temp);
      /*
              set<int> temp;
              temp.insert(*eit);
              insertSet(out_uncover[*vit], temp);
      */
    }
    // Nov 9 10 2010 for tods correction
    vector<int>::iterator tmp_iter =
        find(out_uncover[*vit].begin(), out_uncover[*vit].end(), *vit);
    if (tmp_iter != out_uncover[*vit].end()) {
      out_uncover[*vit].erase(tmp_iter);
    }
  }

  //	displayLabels();

  gettimeofday(&after_time, NULL);
  run_time = (after_time.tv_sec - before_time.tv_sec) * 1000.0 +
             (after_time.tv_usec - before_time.tv_usec) * 1.0 / 1000.0;
  cout << "computing uncover set time:" << run_time << " (ms)" << '\n';

  if (compress) {
    cout << "post-processing on data compression" << '\n';
    gettimeofday(&before_time, NULL);

    double size_ratio = 0.9;
    cout << "init cl=" << g.num_vertices() * size_ratio << '\n';
    int num_cluster =
        (int)(g.num_vertices() * size_ratio > 10 ? g.num_vertices() * size_ratio
                                                 : 10);
    DataComp dc(out_uncover, grts, num_cluster, g.num_vertices());
    dc.comp_kmeans();
    effective = dc.checkSize();
    if (effective) {
      dc.getcomp_data(out_uncover);
      dc.getcomp_table(comp_table);
    }
    /*
    dc.display_compdata();
    dc.display_comptable();
    */

    gettimeofday(&after_time, NULL);
    run_time = (after_time.tv_sec - before_time.tv_sec) * 1000.0 +
               (after_time.tv_usec - before_time.tv_usec) * 1.0 / 1000.0;
    cout << "data compression time:" << run_time << " (ms)" << '\n';
  }
}

// vector v2 inserts into vector v1
void PathTree::mergeVector(vector<int> &v1, vector<int> &v2) {
  if (v2.empty())
    return;
  vector<int> visitstack;
  vector<int> vb;
  vector<int>::iterator vit1 = v1.begin(), vit2 = v2.begin();

  while (vit1 != v1.end() && vit2 != v2.end()) {
    if (*vit1 == *vit2) {
      vb.push_back(*vit1);
      vit1++;
      vit2++;
      continue;
    }

    if (labels[*vit1][0] < labels[*vit2][0]) {
      vb.push_back(*vit1);
      vit1++;
    } else if (labels[*vit1][0] > labels[*vit2][0]) {
      vb.push_back(*vit2);
      vit2++;
    } else {
      if (labels[*vit1][2] < labels[*vit2][2]) {
        vb.push_back(*vit1);
        vit1++;
      } else {
        vb.push_back(*vit2);
        vit2++;
      }
    }
  }
  if (vit1 != v1.end()) {
    for (; vit1 != v1.end(); vit1++)
      vb.push_back(*vit1);
  }
  if (vit2 != v2.end()) {
    for (; vit2 != v2.end(); vit2++)
      vb.push_back(*vit2);
  }

  int vid;
  vector<bool> mark = vector<bool>(vb.size(), true);
  int i;
  for (vit1 = vb.begin(), i = 0; vit1 != vb.end(); vit1++, i++) {
    if (visitstack.empty())
      visitstack.push_back(*vit1);
    else {
      while (!visitstack.empty()) {
        vid = visitstack.back();
        if (!(labels[vid][0] <= labels[*vit1][0] &&
              labels[vid][1] >= labels[*vit1][1]))
          visitstack.pop_back();
        else
          break;
      }
      if (!visitstack.empty()) {
        vid = visitstack.back();
        if (labels[vid][0] <= labels[*vit1][0] &&
            labels[vid][1] >= labels[*vit1][1] &&
            labels[vid][2] <= labels[*vit1][2]) {
          mark[i] = false;
        } else
          visitstack.push_back(*vit1);
      } else
        visitstack.push_back(*vit1);
    }
  }

  v1 = vector<int>();
  for (i = 0; i < vb.size(); i++) {
    if (mark[i])
      v1.push_back(vb[i]);
  }
}

void PathTree::insertSet(set<int> &s1, set<int> &s2) {
  set<int>::iterator sit1, sit2;
  bool insert;
  int pre1, post1, pre2, post2;
  for (sit2 = s2.begin(); sit2 != s2.end(); sit2++) {
    insert = true;
    pre2 = labels[*sit2][0];
    post2 = labels[*sit2][1];
    for (sit1 = s1.begin(); sit1 != s1.end();) {
      pre1 = labels[*sit1][0];
      post1 = labels[*sit1][1];
      if (pre2 >= pre1 && post2 <= post1 &&
          labels[*sit1][2] < labels[*sit2][2]) {
        insert = false;
        break;
      } else if (pre2 <= pre1 && post2 >= post1 &&
                 labels[*sit1][2] > labels[*sit2][2]) {
        s1.erase(*sit1);
        sit1++;
      } else
        sit1++;
    }
    if (insert)
      s1.insert(*sit2);
  }
}

void PathTree::displayLabels() {
  vector<int>::iterator vit;
  vector<int>::iterator sit;
  for (vit = grts.begin(); vit != grts.end(); vit++) {
    vector<int> pset = out_uncover[*vit];
    if (pset.size() <= 0)
      continue;
    sort(pset.begin(), pset.end());
    cout << *vit << ": ";
    for (sit = pset.begin(); sit != pset.end(); sit++)
      cout << *sit << " ";
    cout << '\n';
  }
}

double PathTree::cover_ratio() {
  int gsize = g.num_vertices();

  int counter = 0;
  int pre1, post1, pre2, post2, src, trg;
  double tcsize = 0;
  Graph tc(g.num_vertices());
  GraphUtil::transitive_closure(g, tc);
  EdgeList el;
  EdgeList::iterator eit;
  for (int i = 0; i < tc.num_vertices(); i++) {
    el = tc.out_edges(i);
    tcsize += el.size();

    for (eit = el.begin(); eit != el.end(); eit++) {
      if (i == *eit) {
        counter++;
        continue;
      }
      src = i;
      trg = *eit;
      pre1 = labels[src][0];
      post1 = labels[src][1];
      pre2 = labels[trg][0];
      post2 = labels[trg][1];

      if (labels[src][2] <= labels[trg][2] && post2 >= pre1 && post2 <= post1)
        counter++;
    }
  }

  //	cout << "# TC is covered by ChainTree(PathTree) = " << counter << '\n';

  return (counter * 1.0) / (tcsize * 1.0);
}

double PathTree::compress_ratio() {
  int gsize = g.num_vertices();

  int counter = 0;
  int pre1, post1, pre2, post2, src, trg;
  double tcsize = 0;
  Graph tc(g.num_vertices());
  GraphUtil::transitive_closure(g, tc);
  EdgeList el;
  EdgeList::iterator eit;
  for (int i = 0; i < tc.num_vertices(); i++) {
    el = tc.out_edges(i);
    tcsize += el.size();
  }
  cout << "#TC size = " << tcsize << '\n';
  int uncover_size = 0;
  vector<vector<int>>::iterator vit;
  for (vit = out_uncover.begin(); vit != out_uncover.end(); vit++) {
    uncover_size += vit->size();
  }
  map<int, vector<int>>::iterator mit;
  for (mit = comp_table.begin(); mit != comp_table.end(); mit++)
    uncover_size += mit->second.size();

  return (uncover_size * 1.0) / (tcsize * 1.0);
}

bool PathTree::reach(int src, int trg) {
  if (src == trg)
    return true;

  int pre1, post1, pre2, post2;
  pre1 = labels[src][0];
  post1 = labels[src][1];
  //	pre2 = labels[trg][0];
  post2 = labels[trg][1];

  if (labels[src][2] <= labels[trg][2] && post2 >= pre1 && post2 <= post1)
    return true;

  //	set<int> si = out_uncover[src];
  //	set<int>::iterator sit;
  vector<int> &si = out_uncover[src];
  vector<int>::iterator sit;
  for (sit = si.begin(); sit != si.end(); sit++) {
    pre1 = labels[*sit][0];
    post1 = labels[*sit][1];
    if (labels[*sit][2] <= labels[trg][2] && post2 >= pre1 && post2 <= post1)
      return true;
  }

  return false;
}

bool PathTree::reach_dc(int src, int trg) {
  if (!effective) {
    bool result = reach(src, trg);
    return result;
  }

  int pre1, post1, pre2, post2;
  pre1 = labels[src][0];
  post1 = labels[src][1];
  pre2 = labels[trg][0];
  post2 = labels[trg][1];

  if (labels[src][2] <= labels[trg][2] && post2 >= pre1 && post2 <= post1)
    return true;

  int gsize = g.num_vertices();
  vector<int>::iterator sit;
  vector<int> &si = out_uncover[src];
  vector<int>::iterator vit;
  vector<int> tmp_si;
  vector<int> neg_si;
  int tab_index = 0;
  for (vit = si.begin(); vit != si.end(); vit++) {
    if (*vit >= 0)
      tmp_si.push_back(*vit);
    else if (*vit <= -gsize)
      neg_si.push_back(-(*vit + gsize));
    else
      tab_index = *vit;
  }
  set_difference(comp_table[tab_index].begin(), comp_table[tab_index].end(),
                 neg_si.begin(), neg_si.end(), back_inserter(tmp_si));

  // for test
  //	cout << "query " << src << "->" << trg << '\n';
  for (vit = tmp_si.begin(); vit != tmp_si.end(); vit++) {
    //	if (*vit>=0)
    {
      pre1 = labels[*vit][0];
      post1 = labels[*vit][1];
      if (labels[*vit][2] <= labels[trg][2] && post2 >= pre1 && post2 <= post1)
        return true;
    }
  }

  return false;
}

// for test
bool PathTree::test_reach(int src, int trg) {
  bool r = reach(src, trg);
  if (r != tcm[make_pair(src, trg)]) {
    cout << "Wrong: [" << src << "] to [" << trg << "] reach = " << r << '\n';
    return false;
  }

  return true;
}

bool PathTree::test_reach_dc(int src, int trg) {
  bool r = reach_dc(src, trg);
  if (r != tcm[make_pair(src, trg)]) {
    cout << "Wrong: [" << src << "] to [" << trg << "] reach = " << r << '\n';
    return false;
  }

  return true;
}

void PathTree::save_labels(ofstream &label_file) {
  label_file << g.num_vertices() << " " << 1 << " " << 1 << " " << '\n';

  for (int vid = 0; vid < out_uncover.size(); vid++) {
    vector<int> &si = out_uncover[vid];
    label_file << vid << ": ";
    for (auto sit = si.begin(); sit != si.end(); sit++) {
      label_file << (*sit) << " ";
    }
    label_file << '#' << '\n';
  }
  label_file << '\n';
  for (int i = 0; i < g.num_vertices(); i++) {
    label_file << i << ": ";
    for (int j = 0; j < 3; j++) {
      label_file << labels[i][j] << " ";
    }
    label_file << '\n';
  }
  label_file.close();
}
