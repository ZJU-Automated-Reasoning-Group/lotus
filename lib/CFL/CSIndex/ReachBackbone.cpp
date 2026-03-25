/**
 * @file ReachBackbone.cpp
 * @brief Implementation of backbone discovery algorithm for CSIndex.
 *
 * The ReachBackbone class implements the core algorithm for discovering a
 * minimal set of "backbone" vertices (gates) that enable efficient reachability
 * queries.
 *
 * Key concepts:
 * - Backbone vertices: A subset of vertices that capture most reachability
 * relationships
 * - Gate graph: A compressed graph built over backbone vertices
 * - Materialization: Precompute local neighborhoods for high-degree vertices
 *
 * Algorithm overview:
 * 1. Pre-select high-degree vertices as initial backbone
 * 2. Materialize local neighborhoods for hub vertices
 * 3. For each candidate vertex, check if it's needed as a backbone vertex
 * 4. Build gate graph connecting backbone vertices within epsilon+1 distance
 *
 * The backbone discovery uses a "life" propagation algorithm to determine
 * whether a vertex is necessary for maintaining reachability coverage.
 *
 * Reference: OOPSLA 2022a paper on Extended Dyck-CFL Reachability indexing.
 */

#include "CFL/CSIndex/ReachBackbone.h"

/**
 * @brief Constructor for ReachBackbone.
 *
 * Initializes data structures for backbone discovery:
 * - gates: Bit vector marking backbone vertices
 * - materialized: Bit vector marking vertices with precomputed neighborhoods
 * - localneighbors: Precomputed in/out neighborhoods for materialized vertices
 * - Work queues and distance/life tracking for BFS-based algorithms
 *
 * @param graph Reference to the input graph
 * @param r Epsilon radius for local gate computation
 * @param ratio Pre-selection ratio for initial backbone vertices
 * @param _level Hierarchy level (for multi-level indexing)
 */
ReachBackbone::ReachBackbone(Graph &graph, int r, double ratio, int _level)
    : g(graph), epsilon(r), preselectratio(ratio), level(_level) {
  gsize = g.num_vertices();
  ref = 0;   // Reference value for marking visited vertices in BFS
  opCnt = 0; // Operation counter for visited tracking
  bbedgesize = 0;
  gates = new bit_vector(gsize);
  materialized = new bit_vector(gsize);
  localneighbors =
      vector<vector<bit_vector *>>(gsize, vector<bit_vector *>(2, NULL));
  que = vector<int>(gsize, 0);     // Primary BFS queue
  que2 = vector<int>(gsize, 0);    // Secondary queue for rescanning
  dist = vector<int>(gsize, 0);    // Distance from current source
  life = vector<int>(gsize, 0);    // "Life" value for reachability propagation
  visited = vector<int>(gsize, 0); // Visit markers
  blocknum = 10;                   // Number of blocks for block ordering
}

ReachBackbone::~ReachBackbone() {
  delete gates;
  for (int i = 0; i < gsize; i++) {
    if (materialized->get(i)) {
      if (localneighbors[i][0] != NULL)
        delete localneighbors[i][0];
      if (localneighbors[i][1] != NULL)
        delete localneighbors[i][1];
    }
  }
  delete materialized;
}

void ReachBackbone::setBlockNum(int _bn) { blocknum = _bn; }

set<int> ReachBackbone::getBBvertices() const { return bbvertices; }

void ReachBackbone::setbbvertex(set<int> bbv) {
  bbvertices.insert(bbv.begin(), bbv.end());
  set<int>::iterator sit;
  for (sit = bbv.begin(); sit != bbv.end(); sit++)
    gates->set_one(*sit);
}

void ReachBackbone::setMaterializedNodes(vector<int> hubnodes) {
  vector<int>::iterator vit;
  for (vit = hubnodes.begin(); vit != hubnodes.end(); vit++) {
    bbvertices.insert(*vit);
    gates->set_one(*vit);
    if (!materialized->get(*vit)) {
      materialized->set_one(*vit);
      localneighbors[*vit][0] = new bit_vector(gsize);
      MaterializeNeighbors(*vit, epsilon, false, localneighbors[*vit][0]);
      localneighbors[*vit][1] = new bit_vector(gsize);
      MaterializeNeighbors(*vit, epsilon, true, localneighbors[*vit][1]);
    }
  }
}

int ReachBackbone::BFSNeighbors(int vid, int step, bool out) {
  int begin = 0, end = 0, u, val, nid, num = 0;
  que[end++] = vid;
  num++;
  ref += step + 1;
  dist[vid] = ref;
  EdgeList el;
  EdgeList::iterator eit;
  while (begin < end) {
    u = que[begin++];
    val = dist[u];
    if (out)
      el = g.out_edges(u);
    else
      el = g.in_edges(u);
    for (eit = el.begin(); eit != el.end(); eit++) {
      nid = (*eit);
      if (dist[nid] < ref) {
        dist[nid] = val + 1;
        num++;
        if (val + 1 - ref < step)
          que[end++] = nid;
      }
    }
  }
  return num;
}

int ReachBackbone::MaterializeNeighbors(int vid, int step, bool out,
                                        bit_vector *neighbors) {
  int begin = 0, end = 0, u, val, nid, num = 0;
  que[end++] = vid;
  num++;
  ref += step + 1;
  dist[vid] = ref;
  EdgeList el;
  EdgeList::const_iterator eit;
  while (begin < end) {
    u = que[begin++];
    val = dist[u];
    if (out)
      el = g.out_edges(u);
    else
      el = g.in_edges(u);
    for (eit = el.begin(); eit != el.end(); eit++) {
      nid = (*eit);
      if (dist[nid] < ref) {
        dist[nid] = val + 1;
        num++;
        neighbors->set_one(nid);
        if (val + 1 - ref < step)
          que[end++] = nid;
      }
    }
  }
  return num;
}

void ReachBackbone::blockOrdering(vector<int> &ranks) {
  cout << "block ordering..." << '\n';
  vector<int> ts;
  GraphUtil::topological_sort(g, ts);
  multimap<int, int> rankmap;
  multimap<int, int>::reverse_iterator riter;
  int blocksize = gsize / blocknum + 1;
  int begin = 0, end, in, out, val;
  ranks.clear();
  while (begin < gsize) {
    end = min(begin + blocksize, gsize);
    rankmap.clear();
    for (int i = begin; i < end; i++) {
      in = g.in_degree(ts[i]);
      out = g.out_degree(ts[i]);
      in = max(1, in);
      val = in * out;
      rankmap.insert(make_pair(val, ts[i]));
    }
    for (riter = rankmap.rbegin(); riter != rankmap.rend(); riter++) {
      ranks.push_back(riter->second);
    }
    begin = end;
  }
}

// ranking tyep: 0 (in-degree) 1 (out-degree) 2 (in-degree*outdegree)
void ReachBackbone::vertexRanking(vector<int> &ranks, int type) {
  multimap<int, int> rankmap;
  multimap<int, int>::reverse_iterator riter;
  int val, in, out;
  if (type == 6) {
    blockOrdering(ranks);
    return;
  }
  for (int i = 0; i < gsize; i++) {
    switch (type) {
    case 0:
      val = g.in_degree(i);
      break;
    case 1:
      val = g.out_degree(i);
      break;
    case 2:
      in = g.in_degree(i);
      out = g.out_degree(i);
      in = max(1, in);
      //	out = max(1,out);
      val = in * out;
      break;
    case 3:
      val = BFSNeighbors(i, 2, false);
      break;
    case 4:
      val = BFSNeighbors(i, 2, true);
      break;
    case 5:
      in = BFSNeighbors(i, 2, false);
      out = BFSNeighbors(i, 2, true);
      val = in * out;
      break;
    }
    rankmap.insert(make_pair(val, i));
  }

  ranks.clear();
  for (riter = rankmap.rbegin(); riter != rankmap.rend(); riter++) {
    ranks.push_back(riter->second);
  }
}

/**
 * @brief Determine if a vertex should be selected as a backbone vertex.
 *
 * This is the core decision function for backbone discovery. It uses a
 * BFS-based algorithm with "life" propagation to determine if vertex vid is
 * necessary:
 *
 * Algorithm:
 * 1. Perform BFS from vid up to epsilon+1 distance
 * 2. Propagate "life" values: vertices reachable from gates get life >= ref
 * 3. Check if there are local neighbors at distance epsilon+1 that:
 *    - Are not covered by existing materialized hubs
 *    - Have life < ref (not reachable from existing gates)
 *
 * A vertex is selected as backbone if it provides unique coverage for
 * vertices that would otherwise be unreachable through the current backbone.
 *
 * @param vid Vertex ID to check
 * @return true if vid should be a backbone vertex, false otherwise
 */
bool ReachBackbone::backboneByNode(int vid) {
#ifdef RBDEBUG
  cout << "here check " << vid << '\n';
  cout << "gates: ";
  set<int>::iterator iter;
  for (iter = bbvertices.begin(); iter != bbvertices.end(); iter++)
    cout << *iter << " ";
  cout << '\n';
#endif

  bool updated = false, touch = false; // touch is used to check wheather there
                                       // is epsilon+1 local neighbors
  int begin = 0, end = 0, begin2 = 0, end2 = 0, u, val, lifeval, nid, index,
      liveneighbors = 0;
  vector<int> localneis, hubnodes;
  que[end++] = vid;
  opCnt++;
  ref += epsilon + 2;
  dist[vid] = ref;
  EdgeList el;
  EdgeList::iterator eit;
#ifdef RBDEBUG
  cout << "ref=" << ref << " opCnt=" << opCnt << '\n';
#endif
  while (begin < end) {
    u = que[begin++];
    visited[u] = opCnt;
#ifdef RBDEBUG
    cout << "visit " << u << " life=" << life[u] << " dist=" << dist[u] << '\n';
#endif
    if (gates->get(u))
      life[u] = ref; // NR
    if (dist[u] == ref + epsilon + 1) {
      touch = false;
      if (life[u] < ref) {
        for (index = 0; index < hubnodes.size(); index++) {
          if (localneighbors[hubnodes[index]][1]->get(u))
            break;
        }
        if (index >= hubnodes.size()) {
          localneis.push_back(u);
#ifdef RBDEBUG
          cout << "localneis insert " << u << " life=" << life[u]
               << " dist=" << dist[u] << '\n';
#endif
        }
      }
      //			continue;  //NR
    } else {
      if (life[u] < ref) {
        for (index = 0; index < hubnodes.size(); index++) {
          if (localneighbors[hubnodes[index]][1]->get(u))
            break;
        }
        if (index >= hubnodes.size())
          liveneighbors++;
      }
    }
    //		if (gates->get(u)) life[u] = ref; //NR
#ifdef RBDEBUG
    cout << "after update visit " << u << " life=" << life[u]
         << " dist=" << dist[u] << '\n';
#endif
    val = dist[u];
    lifeval = life[u];
    el = g.out_edges(u);
    for (eit = el.begin(); eit != el.end(); eit++) {
      nid = (*eit);
      // reach the lowest level NR
      if (val == ref + epsilon + 1) {
#ifdef RBDEBUG
        cout << "\tReach lowest level" << '\n';
#endif
        if (dist[nid] < ref)
          continue;
      }
#ifdef RBDEBUG
      cout << "\ttouch " << nid << " life=" << life[nid]
           << " dist=" << dist[nid] << '\n';
#endif
      // update life time
      updated = false;
      if (lifeval >= ref && lifeval < epsilon + ref) {
        if (life[nid] > lifeval + 1 || life[nid] < ref) {
#ifdef RBDEBUG
          cout << "update " << nid << "'s life from " << life[nid] << " to "
               << lifeval + 1 << '\n';
#endif
          life[nid] = lifeval + 1;
          updated = true;
        }
      }
      // update distance
      if (dist[nid] < ref) {
        dist[nid] = val + 1;
        if (val + 1 - ref <= epsilon + 1) {
          if (materialized->get(nid)) { //  && val+1-ref<=epsilon) {   //NR
            hubnodes.push_back(
                nid); // it is not helpful if hubnode in the epsilon+1 level
#ifdef RBDEBUG
            cout << "==find hubnode " << nid << '\n';
#endif
          } else {
            que[end++] = nid;
#ifdef RBDEBUG
            cout << "=====insert " << nid << " to current que" << '\n';
#endif
          }
          /*
          if (!materialized->get(nid))
                  que[end++] = nid;
          else if (val+1-ref<=epsilon)
                  hubnodes.push_back(nid);
          */
        }
      } else if (updated && visited[nid] == opCnt) {
        // need to rescan and propogate life
        if (!materialized->get(nid))
          que2[end2++] = nid;
#ifdef RBDEBUG
        cout << "=====insert " << nid << " to que2 for further update" << '\n';
#endif
      }
    }
  }

#ifdef RBDEBUG
  cout << "*******start rescan procedure*******" << '\n';
#endif

  // iteratively rescan the vertex influenced by reversed edges with respect to
  // BFS tree
  bool useque2 = true;
  begin = 0;
  end = 0;
  opCnt++;
  while (begin < end || begin2 < end2) {
    if (useque2)
      u = que2[begin2++];
    else
      u = que[begin++];
#ifdef RBDEBUG
    cout << " second visit " << u << " life=" << life[u] << " dist=" << dist[u]
         << " useque2=" << useque2 << '\n';
#endif
    //		if (life[u]-ref==epsilon || dist[u]==ref+epsilon+1) {  // NR
    if (useque2 && begin2 >= end2) {
      begin2 = 0;
      end2 = 0;
      useque2 = false;
      opCnt++;
#ifdef RBDEBUG
      cout << "===========================switch to que1" << '\n';
#endif
      continue; // NR
    } else if (!useque2 && begin >= end) {
      begin = 0;
      end = 0;
      useque2 = true;
      opCnt++;
#ifdef RBDEBUG
      cout << "===========================switch to que2" << '\n';
#endif
      continue; // NR
    }
    //			continue; // NR
    //		}  // NR
    val = dist[u];
    lifeval = life[u];
    el = g.out_edges(u);
    for (eit = el.begin(); eit != el.end(); eit++) {
      nid = (*eit);
      // reach the lowest level NR
      if (val == ref + epsilon + 1) {
#ifdef RBDEBUG
        cout << "\tReach lowest level and check its available neighbors"
             << '\n';
#endif
        if (dist[nid] < ref)
          continue;
      }
#ifdef RBDEBUG
      cout << "\tsecond touch " << nid << " life=" << life[nid]
           << " dist=" << dist[nid] << '\n';
#endif
      // update life time
      updated = false;
      if (lifeval >= ref && lifeval < epsilon + ref) {
        if (life[nid] > lifeval + 1 || life[nid] < ref) {
#ifdef RBDEBUG
          cout << "second update " << nid << "'s life from " << life[nid]
               << " to " << lifeval + 1 << '\n';
#endif
          life[nid] = lifeval + 1;
          updated = true;
        }
      }
      // update corresponding queue
      //			if (updated && dist[nid]<=epsilon+ref) { //NR
      if (updated && dist[nid] <= epsilon + ref + 1) { // NR
        if (dist[nid] > dist[u]) {
#ifdef RBDEBUG
          cout << "\tsecond insert " << nid << " into que useque2=" << useque2
               << '\n';
#endif
          if (!materialized->get(nid)) {
            if (useque2)
              que2[end2++] = nid;
            else
              que[end++] = nid;
          }
        } else if (visited[nid] != opCnt) {
#ifdef RBDEBUG
          cout << "\tsecond further update " << nid
               << " into que useque2=" << useque2 << '\n';
#endif
          // need to rescan and propogate life
          if (!materialized->get(nid)) {
            if (!useque2)
              que2[end2++] = nid;
            else
              que[end++] = nid;
            visited[nid] = opCnt;
          }
        }
      }
    }
    // change queue and init data structure
    if (useque2 && begin2 >= end2) {
      begin2 = 0;
      end2 = 0;
      useque2 = false;
      opCnt++;
#ifdef RBDEBUG
      cout << "===========================switch to que1" << '\n';
#endif
    } else if (!useque2 && begin >= end) {
      begin = 0;
      end = 0;
      useque2 = true;
      opCnt++;
#ifdef RBDEBUG
      cout << "===========================switch to que2" << '\n';
#endif
    }
  }

// check local neighbors
#ifdef RBDEBUG
  cout << "local neighbors: ";
#endif
  for (int i = 0; i < localneis.size(); i++) {
#ifdef RBDEBUG
    cout << localneis[i] << "|life" << life[localneis[i]] << "|dist"
         << dist[localneis[i]] << " ";
#endif
    for (index = 0; index < hubnodes.size(); index++) {
      if (localneighbors[hubnodes[index]][1]->get(localneis[i]))
        break;
    }
    if (index < hubnodes.size())
      continue;
    if (life[localneis[i]] < ref || life[localneis[i]] - ref > epsilon) {
#ifdef RBDEBUG
      cout << '\n';
#endif
      return true;
    }
  }
#ifdef RBDEBUG
  cout << '\n';
#endif
  // check its potential importance
  /*
  int pval = liveneighbors*g.in_degree(vid);
  if (pval>=4000) return true;
  */
  return false;
}

/**
 * @brief Discover backbone vertices using ranking-based selection.
 *
 * The backbone discovery process:
 * 1. Rank vertices by importance (in-degree, out-degree, or product)
 * 2. Pre-select top vertices by degree as initial backbone
 * 3. Materialize high-degree vertices (hubs) for fast neighborhood queries
 * 4. Iterate through ranked vertices, selecting additional backbone vertices
 *    that provide unique reachability coverage
 *
 * @param type Ranking type:
 *   0 = in-degree, 1 = out-degree, 2 = in*out,
 *   3 = in-neighbors(2), 4 = out-neighbors(2), 5 = in*out neighbors,
 *   6 = block ordering
 */
void ReachBackbone::backboneDiscovery(int type) {
  vector<int> ranks;
  vector<int>::iterator iter;
  cout << "vertex ranking with type=" << type << "..." << '\n';
  vertexRanking(ranks, type);
  cout << "preselecting highest in-degree and out-degree vertices with ratio="
       << preselectratio << "..." << '\n';
  int size = (int)(gsize * preselectratio), inval, outval;
  size = min(size, gsize);
  cout << "preselecting " << size << " vertices..." << '\n';
  multimap<int, int> indegmap, outdegmap, inoutmap;
  multimap<int, int>::reverse_iterator inrit, outrit, inoutit;
  for (int i = 0; i < gsize; i++) {
    inval = g.in_degree(i);
    outval = g.out_degree(i);
    indegmap.insert(make_pair(inval, i));
    outdegmap.insert(make_pair(outval, i));
    inval = max(1, inval);
    outval = max(1, outval);
    inoutmap.insert(make_pair(inval * outval, i));
  }
  inrit = indegmap.rbegin();
  outrit = outdegmap.rbegin();
  inoutit = inoutmap.rbegin();
  int index = 0;
  for (; inrit != indegmap.rend() && outrit != outdegmap.rend() && index < size;
       inrit++, outrit++, inoutit++, index++) {
    bbvertices.insert(inrit->second);
    bbvertices.insert(outrit->second);
    bbvertices.insert(inoutit->second);
    gates->set_one(inrit->second);
    gates->set_one(outrit->second);
    gates->set_one(inoutit->second);
  }
  inoutmap.clear();
  int hubsize = min(200, gsize);
  int hubcounter = 0;
  cout << "materializing hub vertices with in-degree or out-degree larger than "
          "400..."
       << '\n';
  inrit = indegmap.rbegin();
  outrit = outdegmap.rbegin();
  index = 0;
  for (; inrit != indegmap.rend() && outrit != outdegmap.rend() &&
         index < hubsize;
       inrit++, outrit++, index++) {
    bbvertices.insert(inrit->second);
    bbvertices.insert(outrit->second);
    gates->set_one(inrit->second);
    gates->set_one(outrit->second);
    if (!materialized->get(inrit->second) && inrit->first >= 400) {
      materialized->set_one(inrit->second);
      localneighbors[inrit->second][0] = new bit_vector(gsize);
      MaterializeNeighbors(inrit->second, epsilon, false,
                           localneighbors[inrit->second][0]);
      localneighbors[inrit->second][1] = new bit_vector(gsize);
      MaterializeNeighbors(inrit->second, epsilon, true,
                           localneighbors[inrit->second][1]);
      hubcounter++;
    }
    if (!materialized->get(outrit->second) && outrit->first >= 400) {
      materialized->set_one(outrit->second);
      localneighbors[outrit->second][1] = new bit_vector(gsize);
      MaterializeNeighbors(outrit->second, epsilon, true,
                           localneighbors[outrit->second][1]);
      localneighbors[outrit->second][0] = new bit_vector(gsize);
      MaterializeNeighbors(outrit->second, epsilon, false,
                           localneighbors[outrit->second][0]);
      hubcounter++;
    }
  }
  indegmap.clear();
  outdegmap.clear();
  cout << "materialized " << hubcounter << " hubnodes" << '\n';

  cout << "selecting backbone vertex based on vertex ranking..." << '\n';
  index = 0;
  for (iter = ranks.begin(); iter != ranks.end(); iter++, index++) {
    if (gates->get(*iter))
      continue;
#ifdef RBDEBUG
    cout << "processing node " << *iter << '\n';
#endif
    if (backboneByNode(*iter)) {
      bbvertices.insert(*iter);
      gates->set_one(*iter);
    }
    if (index % 100000 == 0)
      cout << "Processed " << index << " nodes and selected "
           << bbvertices.size() << " backbone nodes" << '\n';

    //	if (*iter==23) exit(0);
  }

  //	// output backbone
  //	outputBackbone(filestem);
  //	cout << "#backbone vertex set=" << bbvertices.size() << '\n';

  /*
  char ch;
  cin >> ch;
  cout << "checkbone=" << checkBackbone() << '\n';
  */
}

void ReachBackbone::outputBackbone(const char *filestem) {
  auto *isgate = new bit_vector(gsize);
  set<int>::iterator sit;
  int pr = (int)(preselectratio * 1000);
  string filestr(filestem);
  // construct levelmap for previous level
  map<int, int> levelmap;
  int size, tmp, radius = epsilon + 1;
  if (level > 1) {
    string preggfile = filestr + "." + to_string(radius) + to_string(pr) +
                       "gates" + to_string(level - 1);
    ifstream levelin(preggfile.c_str());
    cout << "reading ggfile of previous level " << preggfile << '\n';
    levelin >> size >> tmp;
    for (int i = 0; i < size; i++) {
      levelin >> tmp;
      levelmap[i] = tmp;
    }
    levelin.close();
  }
  string indexfile =
      filestr + "." + to_string(radius) + to_string(pr) + "gates";
  cout << "writing backbone nodes to " << indexfile << '\n';
  ofstream out(indexfile.c_str());
  out << bbvertices.size() << "\t" << radius << '\n';
  for (sit = bbvertices.begin(); sit != bbvertices.end(); sit++) {
    isgate->set_one(*sit);
    if (levelmap.empty())
      out << *sit << '\n';
    else
      out << levelmap[*sit] << '\n';
  }
  out.close();

  string ggfile = filestr + "." + to_string(radius) + to_string(pr) + "gg";
  cout << "generating backbone and writing into " << ggfile << '\n';
  ofstream ggout(ggfile.c_str());

  //	cout << "#E(gate graph)=" << genGateGraph(ggout,radius+1,false) << '\n';
  cout << "#V(gate graph)=" << bbvertices.size() << '\n';

  bbedgesize = GraphUtil::buildGateGraphWrite(g, isgate, radius + 1, ggout);

  //	cout << "#E(gate graph): " << GraphUtil::buildGateGraph(g, isgate,
  //radius+1, gategraph) << '\n'; 	gategraph.writeGraph(ggout);

  //	printBBvertex(cout);
  //	gategraph.writeGraph(cout);

  ggout.flush();
  ggout.close();
  delete isgate;
}

int ReachBackbone::genGateGraph(ostream &out, int radius, bool check) {
  set<int>::iterator sit;
  int index = 0;
  map<int, int> gateindex;
  for (int i = 0; i < gsize; i++) {
    if (gates->get(i)) {
      gateindex[i] = index;
      index++;
    }
  }
  if (check) {
    for (int i = 0; i < index; i++) {
      gategraph.addVertex(i);
    }
  }
  out << "graph_for_reach" << '\n';
  out << bbvertices.size() << '\n';
  int edgenum = 0;
  for (sit = bbvertices.begin(); sit != bbvertices.end(); sit++) {
    edgenum += genGateGraphByNode(*sit, radius, out, check, gateindex);
  }
  if (check)
    gategraph.sortEdges();
  return edgenum;
}

int ReachBackbone::genGateGraphByNode(int vid, int radius, ostream &out,
                                      bool check, map<int, int> &gateindex) {
#ifdef RBDEBUG1
  cout << "here check " << vid << '\n';
#endif

  bool updated = false, valid = false;
  int begin = 0, end = 0, begin2 = 0, end2 = 0, u, val, lifeval, nid, index;
  vector<int> localneis, hubnodes;
  que[end++] = vid;
  opCnt++;
  ref += radius + 2;
  dist[vid] = ref;
  EdgeList el;
  EdgeList::iterator eit;
#ifdef RBDEBUG1
  cout << "ref=" << ref << " opCnt=" << opCnt << '\n';
#endif
  while (begin < end) {
    u = que[begin++];
    visited[u] = opCnt;
#ifdef RBDEBUG1
    cout << "visit " << u << " life=" << life[u] << " dist=" << dist[u] << '\n';
#endif
    if (u != vid && gates->get(u) && life[u] < ref) {
      localneis.push_back(u);
    }
    /*
                    if (dist[u]==ref+epsilon+1) {
                            if (life[u]<ref) {
                                    for (index = 0; index < hubnodes.size();
       index++) { if (localneighbors[hubnodes[index]][1]->get(u))	break;
                                    }
                                    if (index>=hubnodes.size()) {
                                            localneis.push_back(u);
                                            #ifdef RBDEBUG
                                            cout << "localneis insert " << u <<
       " life=" << life[u] << " dist=" << dist[u] << '\n'; #endif
                                    }
                            }
                            continue;
                    }
                    if (gates->get(u)) life[u] = ref;
    */
#ifdef RBDEBUG1
    cout << "after update visit " << u << " life=" << life[u]
         << " dist=" << dist[u] << '\n';
#endif
    val = dist[u];
    lifeval = life[u];
    valid = false;
    if ((u != vid) && (lifeval == ref || gates->get(u)))
      valid = true;
    el = g.out_edges(u);
    for (eit = el.begin(); eit != el.end(); eit++) {
      nid = (*eit);
      // reach the lowest level NR
      if (val == ref + radius) {
#ifdef RBDEBUG1
        cout << "\tReach lowest level" << '\n';
#endif
        if (dist[nid] < ref)
          continue;
      }
#ifdef RBDEBUG1
      cout << "\ttouch " << nid << " life=" << life[nid]
           << " dist=" << dist[nid] << '\n';
#endif
      // update life time
      updated = false;
      if (valid) {
        if (life[nid] < ref) {
#ifdef RBDEBUG1
          cout << "update " << nid << "'s life from " << life[nid] << " to "
               << lifeval << '\n';
#endif
          life[nid] = ref;
          updated = true;
        }
      }
      // update distance
      if (dist[nid] < ref) {
        dist[nid] = val + 1;
        if (val + 1 - ref <= radius) {
          if (materialized->get(nid)) { // && val+1-ref<=epsilon) { // NR
            hubnodes.push_back(
                nid); // it is not helpful if hubnode in the epsilon+1 level
#ifdef RBDEBUG1
            cout << "==find hubnode " << nid << '\n';
#endif
          } else {
            que[end++] = nid;
#ifdef RBDEBUG1
            cout << "=====insert " << nid << " to current que" << '\n';
#endif
          }
        }
      } else if (updated && visited[nid] == opCnt) {
        // need to rescan and propogate life
        if (!materialized->get(nid))
          que2[end2++] = nid;
#ifdef RBDEBUG1
        cout << "=====insert " << nid << " to que2 for further update" << '\n';
#endif
      }
    }
  }

#ifdef RBDEBUG1
  cout << "*******start rescan procedure*******" << '\n';
#endif

  // iteratively rescan the vertex influenced by reversed edges with respect to
  // BFS tree
  bool useque2 = true;
  begin = 0;
  end = 0;
  opCnt++;
  while (begin < end || begin2 < end2) {
    if (useque2)
      u = que2[begin2++];
    else
      u = que[begin++];
#ifdef RBDEBUG1
    cout << " second visit " << u << " life=" << life[u] << " dist=" << dist[u]
         << " useque2=" << useque2 << '\n';
#endif
    //		if (life[u]-ref==epsilon || dist[u]==ref+epsilon+1) {
    if (useque2 && begin2 >= end2) {
      begin2 = 0;
      end2 = 0;
      useque2 = false;
      opCnt++;
#ifdef RBDEBUG1
      cout << "===========================switch to que1" << '\n';
#endif
      continue;
    } else if (!useque2 && begin >= end) {
      begin = 0;
      end = 0;
      useque2 = true;
      opCnt++;
#ifdef RBDEBUG1
      cout << "===========================switch to que2" << '\n';
#endif
      continue;
    }
    //			continue;
    //		}
    val = dist[u];
    lifeval = life[u];
    valid = false;
    if ((u != vid) && (lifeval == ref || gates->get(u)))
      valid = true;
    el = g.out_edges(u);
    for (eit = el.begin(); eit != el.end(); eit++) {
      nid = (*eit);
      // reach the lowest level NR
      if (val == ref + radius) {
#ifdef RBDEBUG1
        cout << "\tReach lowest level" << '\n';
#endif
        if (dist[nid] < ref)
          continue;
      }
#ifdef RBDEBUG1
      cout << "\tsecond touch " << nid << " life=" << life[nid]
           << " dist=" << dist[nid] << '\n';
#endif
      // update life time
      updated = false;
      if (valid) {
        if (life[nid] < ref) { // NR life[nid]>lifeval+1 ||
#ifdef RBDEBUG1
          cout << "second update " << nid << "'s life from " << life[nid]
               << " to " << lifeval + 1 << '\n';
#endif
          life[nid] = ref;
          updated = true;
        }
      }
      // update corresponding queue (change to epsilon+ref+1 by Ning)
      if (updated && dist[nid] <= radius + ref) {
        if (dist[nid] > dist[u]) {
#ifdef RBDEBUG1
          cout << "\tsecond insert " << nid << " into que useque2=" << useque2
               << '\n';
#endif
          if (!materialized->get(nid)) {
            if (useque2)
              que2[end2++] = nid;
            else
              que[end++] = nid;
          }
        } else if (visited[nid] != opCnt) {
#ifdef RBDEBUG1
          cout << "\tsecond further update " << nid
               << " into que useque2=" << useque2 << '\n';
#endif
          // need to rescan and propogate life
          if (!materialized->get(nid)) {
            if (!useque2)
              que2[end2++] = nid;
            else
              que[end++] = nid;
            visited[nid] = opCnt;
          }
        }
      }
    }
    // change queue and init data structure
    if (useque2 && begin2 >= end2) {
      begin2 = 0;
      end2 = 0;
      useque2 = false;
      opCnt++;
#ifdef RBDEBUG1
      cout << "===========================switch to que1" << '\n';
#endif
    } else if (!useque2 && begin >= end) {
      begin = 0;
      end = 0;
      useque2 = true;
      opCnt++;
#ifdef RBDEBUG1
      cout << "===========================switch to que2" << '\n';
#endif
    }
  }

// check local neighbors
#ifdef RBDEBUG1
  cout << "local neighbors: ";
#endif
  // insert hubnodes into local neighbors
  for (int i = 0; i < hubnodes.size(); i++)
    localneis.push_back(hubnodes[i]);
  sort(localneis.begin(), localneis.end());

  int edgenum = 0;
  out << gateindex[vid] << ": ";
  for (int i = 0; i < localneis.size(); i++) {
#ifdef RBDEBUG1
    cout << localneis[i] << "|life" << life[localneis[i]] << "|dist"
         << dist[localneis[i]] << " ";
#endif
    if (!materialized->get(localneis[i])) {
      for (index = 0; index < hubnodes.size(); index++) {
        if (localneighbors[hubnodes[index]][1]->get(localneis[i]))
          break;
      }
      if (index < hubnodes.size())
        continue;
    }
    // no other local gates can reach this gate
    if (life[localneis[i]] < ref) {
#ifdef RBDEBUG1
      cout << '\n';
#endif
      if (check)
        gategraph.addEdge(gateindex[vid], gateindex[localneis[i]]);
      out << gateindex[localneis[i]] << " ";
      edgenum++;
    }
  }
#ifdef RBDEBUG1
  cout << '\n';
#endif
  out << "#" << '\n';
  return edgenum;
}

void ReachBackbone::printBBGraph(ostream &out) {
  bit_vector *isgate = new bit_vector(gsize);
  set<int>::iterator sit;
  for (sit = bbvertices.begin(); sit != bbvertices.end(); sit++) {
    isgate->set_one(*sit);
  }
  bbedgesize = GraphUtil::buildGateGraphWrite(g, isgate, epsilon + 1, out);
  delete isgate;
}

void ReachBackbone::printBBvertex(ostream &out) {
  set<int>::iterator sit;
  out << "backbone vertices: ";
  for (sit = bbvertices.begin(); sit != bbvertices.end(); sit++)
    out << *sit << " ";
  out << '\n';
}

int ReachBackbone::getBBsize() const { return bbvertices.size(); }

int ReachBackbone::getBBEdgesize() const { return bbedgesize; }

bool ReachBackbone::checkBackbone() {
  if (gategraph.num_vertices() == 0)
    return false;
  map<int, int> gateindex;
  int index = 0;
  for (int i = 0; i < gsize; i++) {
    if (gates->get(i)) {
      gateindex[i] = index;
      index++;
    }
  }

  /*
  cout << "reach=" << reach(1751,769,gateindex) << '\n';
  if (true) exit(0);
  */
  /*
  for (int i = 0; i < gsize; i++) {
          for (int j = 0; j < gsize; j++) {
                  bool ans = GraphUtil::DFSReachCnt(g, i, j, visited, opCnt);
                  bool r = reach(i,j,gateindex);
                  if (ans!=r) {
                          cout << "wrong " << i << " -> " << j << " r=" << r <<
  '\n'; exit(0);
                  }
          }
  }
  */

  for (int i = 0; i < 100000; i++) {
    int s = lrand48() % gsize;
    int t = lrand48() % gsize;
    if (s != t) {
      cout << "check reach between " << s << " and " << t << " " << i << '\n';
      bool ans = GraphUtil::DFSReachCnt(g, s, t, visited, opCnt);
      bool r = reach(s, t, gateindex);
      if (ans != r) {
        cout << "wrong " << s << " -> " << t << " r=" << r
             << " outdeg=" << g.out_degree(t) << '\n';
        //	char ch;
        //	cin >> ch;
        exit(0);
      }
    }
  }

  return true;
}

bool ReachBackbone::reach(int src, int trg, map<int, int> gateindex) {
  vector<int> out, in;
  if (GraphUtil::BFSOutLocalReach(g, gates, epsilon + 1, src, trg, out))
    return true;
  GraphUtil::collectInLocalGates(g, gates, trg, epsilon + 1, in);
  vector<int>::iterator init, outit;
  /*
  cout << "out[" << src << "]: ";
  for (outit = out.begin(); outit != out.end(); outit++)
          cout << *outit << " ";
  cout << '\n';
  cout << "in[" << trg << "]: ";
  for (init = in.begin(); init != in.end(); init++)
          cout << *init << " ";
  cout << '\n';
  */
  for (outit = out.begin(); outit != out.end(); outit++) {
    for (init = in.begin(); init != in.end(); init++) {
      //		cout << "check " << *outit << " to " << *init << '\n';
      if (GraphUtil::DFSReachCnt(gategraph, gateindex[*outit], gateindex[*init],
                                 visited, opCnt))
        return true;
    }
  }
  return false;
}
