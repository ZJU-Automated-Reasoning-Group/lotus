/**
 * @file Query.cpp
 * @brief Implementation of reachability query interface for CSIndex.
 *
 * This module provides the Query class, which implements various indexing
 * strategies for efficient context-sensitive reachability queries:
 *
 * - DFS: Depth-first search (baseline, no indexing)
 * - GATEDFS: Gate-based DFS with local gate precomputation
 * - GRAIL: Multi-labeling approach for quick rejection
 * - Index-based: Uses precomputed lin/lout labels for fast queries
 *
 * The query system supports:
 * - Materialization: Precompute reachability for high-degree vertices
 * - Local gates: Precompute nearby gate vertices for each node
 * - Multi-labeling: Use multiple DFS labels for probabilistic filtering
 *
 * Query performance is optimized through a combination of:
 * 1. Gate selection: Identify important vertices (gates) for indexing
 * 2. Gate graph: Build a compressed graph over gates
 * 3. Local materialization: Cache reachability for frequently accessed nodes
 */

#include "CFL/CSIndex/Query.h"

#include <cstdlib>
#include <ctime>

/**
 * @brief Constructor with file stem, graph reference, radius, pre-selection
 * ratio, and materialization flag.
 *
 * Initializes a Query object for performing reachability queries. The
 * constructor:
 * 1. Loads gate vertices (important nodes for indexing)
 * 2. Loads the gate graph (compressed graph over gates)
 * 3. Loads precomputed reachability indices (lin/lout labels)
 *
 * @param filestem File stem for I/O operations (used to construct gate/index
 * filenames)
 * @param ig Reference to the input graph
 * @param _r Radius parameter (epsilon) - maximum distance for local gate
 * computation
 * @param _ps Pre-selection ratio - fraction of vertices to pre-select as gates
 * @param mat Materialization flag - whether to use materialized reachability
 *
 * File naming convention:
 *   - Gates: {filestem}.{epsilon}{preselectratio}gates
 *   - Gate graph: {filestem}.{epsilon}{preselectratio}gg
 *   - Index: {filestem}.index
 */
Query::Query(const char *filestem, Graph &ig, int _r, double _ps, bool mat)
    : g(ig) {
  initFlags();
  epsilon = _r;
  preselectratio = _ps;
  ismaterialized = mat;
  string filestr(filestem);
  filestemstr = filestr;
  // init graph
  gsize = g.num_vertices();
  vector<string> gfilenames = makeggfilename(filestem);
  // init gates
  initGates(gfilenames[0].c_str());
  // init gategraph
  initGateGraph(gfilenames[1].c_str());
  // init gate graph's reachability indices
  string indexfile = filestr + ".index";
  initIndex(indexfile.c_str());
  initQueue();
}

// Query::Query(const char* filestem, const char* grafile, int _r) {
//	epsilon = _r;
//	initFlags();
//	string filestr(filestem);
//	filestemstr = filestr;
//	// init graph
//	ifstream in(grafile);
//	if (!in) {
//		cout << "Error: Cannot open " << grafile << '\n';
//		return;
//	}
//	else
//		cout << "reading " << grafile << '\n';
//	g = Graph(in);
//	in.close();
//	gsize = g.num_vertices();
//	// init gates
//	string gatefile = filestr+"."+to_string(epsilon)+"gates";
//	initGates(gatefile.c_str());
//	// init gategraph
//	string ggfile = filestr+"."+to_string(epsilon)+"gg";
//	initGateGraph(ggfile.c_str());
//	// init gate graph's reachability indices
//	string indexfile = filestr+".index";
//	initIndex(indexfile.c_str());
//	initQueue();
// }

// Query::Query(const char* gatefile, const char* ggfile,
//		const char* indexfile, const char* grafile) {
//	initFlags();
//	// init graph
//	ifstream in(grafile);
//	if (!in) {
//		cout << "Error: Cannot open " << grafile << '\n';
//		return;
//	}
//	else
//		cout << "reading " << grafile << '\n';
//	g = Graph(in);
//	in.close();
//	gsize = g.num_vertices();
//	initGates(gatefile);
//	initGateGraph(ggfile);
////	initIndex(indexfile);
//	cout << "Init auxiliary data structures..." << '\n';
//	initQueue();
//}

Query::~Query() {
  if (method_name != "DFS")
    delete gates;

  // release memory
  if (ismaterialized) {
    for (int i = 0; i < gsize; i++) {
      if (materialized->get(i))
        delete inneigs[i];
    }
  }
  delete materialized;
  cout << "reachtime=" << reachtime << '\n';
  cout << "average reachtime=" << (reachtime * 1.0) / (1.0 * 100000) << '\n';
}

int Query::getGateSize() const { return gatesize; }

string Query::getFilestem() const { return filestemstr; }

void Query::setMethodName(string _method_name) { method_name = _method_name; }

string Query::getMethodName() const { return method_name; }

long Query::getIndexSize() const {
  if (method_name == "DFS")
    return 0;
  long size = 0;
  for (int i = 0; i < gsize; i++) {
    if (method_name != "GATEDFS" && method_name != "GRAIL") {
      if (indextype == 0 || indextype == 2)
        size += lin[i].size();
      if (indextype == 1 || indextype == 2)
        size += lout[i].size();
      if (labeltype != 0)
        size += labels[i].size();
    }
  }
  // using gate materialization
  long localgatesize = 0;
  if (useLocalGates || usePartialLocalGates) {
    for (int i = 0; i < gsize; i++) {
      localgatesize += localgates[i].size();
    }
  }
  long multilabels = 0;
  if (useMultiLabels) {
    multilabels = gatesize * graillabels[0].size();
  }
  size += localgatesize;
  size += multilabels;
  return size;
}

vector<long> Query::indexSize() const {
  vector<long> index; // indexsize, grailsize, inoutgates size, totalsize,
                      // inneighbors, inneitotoalsize
  long indexsize = 0, graillabelsize = 0, inoutgatesize = 0, subtotalsize = 0;
  long inneigsize = 0, totalsize = 0;
  if (method_name == "DFS")
    return vector<long>(5, 0);

  for (int i = 0; i < gsize; i++) {
    if (method_name != "GATEDFS" && method_name != "GRAIL") {
      if (indextype == 0 || indextype == 2)
        indexsize += lin[i].size();
      if (indextype == 1 || indextype == 2)
        indexsize += lout[i].size();
      if (labeltype != 0)
        indexsize += labels[i].size();
    }
    if (ismaterialized) {
      if (materialized->get(i))
        inneigsize += inneigs[i]->num_ones();
      inoutgatesize += inoutgates[i][0].size() + inoutgates[i][1].size();
    }
  }
  if (useGlobalMultiLabels)
    graillabelsize = dim * gsize * 2;
  subtotalsize = indexsize + graillabelsize + inoutgatesize;
  totalsize = subtotalsize + inneigsize;
  index.push_back(indexsize);
  index.push_back(graillabelsize);
  index.push_back(inoutgatesize);
  index.push_back(subtotalsize);
  // index.push_back(inneigsize); index.push_back(totalsize);
  index.push_back(reachtime);
  return index;
}

void Query::initFlags() {
  gsize = 0;
  gatesize = 0;
  useLocalGates = false;
  usePartialLocalGates = false;
  useMultiLabels = false;
  useTopoOrder = false;
  num_bits = 1;
  visitnum = 0;
  ref = 0;
  QueryCnt = 0;
  ismaterialized = false;
  useGlobalMultiLabels = false;
  dim = 5;
  preselectratio = 0.05;
  reachtime = 0;
  method_name = "DFS";
}

// init queue and basic data structure "materialized" to maintain correctness
void Query::initQueue() {
  dist = vector<int>(gsize, 0);
  que = vector<int>(gsize, 0);
  visited = vector<int>(gsize, 0);
  materialized = new bit_vector(gsize);
}

void Query::setRadius(int _r) { epsilon = _r; }

int Query::getGateEdgeSize() {
  return gateedgesize;
  //	return gategraph.num_edges();
}

vector<string> Query::makeggfilename(const char *filestem) {
  cout << "preselectratio=" << preselectratio << '\n';
  string filestr(filestem);
  int ps = (int)(preselectratio * 1000);
  filestr += "." + to_string(epsilon) + to_string(ps);
  string gatefilename = filestr + "gates";
  //	std::cout << "gates file name: " << gatefilename << '\n';
  string ggfilename = filestr + "gg";
  //	std::cout << "gg file name: " << ggfilename << '\n';
  vector<string> result;
  result.push_back(gatefilename);
  result.push_back(ggfilename);
  return result;
}

void Query::initGateGraph(const char *ggfilename) {
  ifstream infile(ggfilename);
  if (!infile) {
    cout << "Error: Cannot open " << ggfilename << '\n';
    exit(-1);
  }
  gategraph = Graph(infile);
  infile.close();
  gateedgesize = gategraph.num_edges();
  cout << "#V(gate graph)=" << gategraph.num_vertices()
       << " #E(gate graph)=" << gategraph.num_edges() << '\n';
}

void Query::initGates(const char *gatefile) {
  gates = new bit_vector(gsize);
  ifstream in(gatefile);
  if (!in) {
    cout << "initGates Error: Cannot open " << gatefile << '\n';
    exit(-1);
  } else
    cout << "reading " << gatefile << '\n';
  // fist line: the number of gates
  in >> gatesize >> radius;
  //	cout << "radius=" << radius << '\n';
  num_bits = (int)ceil(log(radius + 1) / log(2));
  int inputval;
  for (int i = 0; i < gatesize; i++) {
    in >> inputval;
    gates->set_one(inputval);
    gatemap[inputval] = i;
  }
  in.close();
  //	displayGates(cout);
}

void Query::initIndex(const char *indexfile) {
  ifstream in(indexfile);
  if (!in) {
    cout << "Error: Cannot open " << indexfile << '\n';
    exit(-1);
  } else
    cout << "reading " << indexfile << '\n';
  int numgates = -1, begin, end;
  labeltype = -1, indextype = -1;
  // first line: #gates #hasLabel #indextype
  in >> numgates >> labeltype >> indextype;
  cout << "numgates: " << numgates << '\n';
  cout << "gatesize: " << gatesize << '\n';
  assert(numgates == gatesize);
  if (indextype == 0) {
    lin = vector<vector<int>>(gsize, vector<int>());
  } else if (indextype == 1) {
    lout = vector<vector<int>>(gsize, vector<int>());
  } else if (indextype == 2) {
    lin = vector<vector<int>>(gsize, vector<int>());
    lout = vector<vector<int>>(gsize, vector<int>());
  }
  int idx, vid;
  string buf, inbuf, sub;
  vector<int> gateindex;
  for (int i = 0; i < gsize; i++) {
    if (gates->get(i))
      gateindex.push_back(i);
  }
  // process lin and lout
  if (indextype != 3) {
    getline(in, buf);
    for (int i = 0; i < gatesize; i++) {
      getline(in, buf);
      // parse inlabels
      //			cout << "lin" << '\n';
      begin = buf.find(":");
      end = buf.find_first_of("#");
      inbuf = buf.substr(begin + 2, end - begin - 2);
      int sid = gateindex[i];
      vector<string> neighbors = Graph::split(inbuf, ' ');
      //			for (int j = 0; j < neighbors.size(); j++) {
      //				if (neighbors[j]=="") continue;
      //				vid = atoi(neighbors[j].c_str());
      //				lin[sid].push_back(gateindex[vid]);
      //			}
      /*
      buf = buf.substr(buf.find_first_of(":")+2);
      // parse lin
      inbuf = buf.substr(0,buf.find_first_of("#"));
      while (inbuf.find(" ")!=string::npos) {
              sub = inbuf.substr(0,inbuf.find(" "));
              istringstream(sub) >> vid;
              lin[gateindex[i]].push_back(gateindex[vid]);
              inbuf.erase(0,inbuf.find(" ")+1);
      }
      */
      //			if (indextype==0 || indextype==2)
      //				sort(lin[sid].begin(),lin[sid].end());
      //			cout << "lout" << '\n';

      // parse lout
      //			begin = end+2;
      //			end = buf.find_last_of("#");
      //			if (end-begin<=0) continue;
      //			buf = buf.substr(begin,end-begin);
      //			neighbors.clear();
      //			neighbors = Graph::split(buf, ' ');
      for (int j = 0; j < neighbors.size(); j++) {
        if (neighbors[j] == "")
          continue;
        vid = atoi(neighbors[j].c_str());
        lout[sid].push_back(gateindex[vid]);
      }
      /*
      buf.erase(0, buf.find_first_of("#")+2);
      while (buf.find(" ")!=string::npos) {
              sub = buf.substr(0,buf.find(" "));
              istringstream(sub) >> vid;
              lout[gateindex[i]].push_back(gateindex[vid]);
              buf.erase(0,buf.find(" ")+1);
      }
      */
      //			if (indextype==1 || indextype==2)
      //				sort(lout[sid].begin(),lout[sid].end());
    }
  }
  if (labeltype == 0) {
    in.close();
    return;
  }
  // process labels
  labels = vector<vector<int>>(gsize, vector<int>());
  getline(in, buf);
  for (int i = 0; i < gatesize; i++) {
    getline(in, buf);
    buf = buf.substr(buf.find_first_of(":") + 2);
    while (buf.find(" ") != string::npos) {
      sub = buf.substr(0, buf.find(" "));
      istringstream(sub) >> vid;
      labels[gateindex[i]].push_back(vid);
      buf.erase(0, buf.find(" ") + 1);
    }
  }
  in.close();
}

// void Query::outIndex(const char* out_index_file){
//	ofstream outer(out_index_file);
//	for
// }

void Query::computeLocalGates(bool ispartial) {
  vector<int> nodes;
  if (ispartial) {
    usePartialLocalGates = true;
    selectPartialNodes(nodes, (int)(gsize * 0.1));
  } else {
    useLocalGates = true;
    selectPartialNodes(nodes, gsize);
  }
  localgates =
      vector<vector<vector<int>>>(gsize, vector<vector<int>>(2, vector<int>()));
  for (int i = 0; i < nodes.size(); i++) {
    GraphUtil::collectInLocalGates(g, gates, nodes[i], radius,
                                   localgates[nodes[i]][0]);
    GraphUtil::collectOutLocalGates(g, gates, nodes[i], radius,
                                    localgates[nodes[i]][0]);
  }
}

void Query::computeMultiLabelsQuickRej(int num_labels) {
  mutipleLabeling(num_labels);
}

// default strategy: randomly select #num vertices
void Query::selectPartialNodes(vector<int> &nodes, int num) {
  assert(num <= gsize);
  nodes.clear();
  if (num == gsize) {
    for (int i = 0; i < gsize; i++)
      nodes.push_back(i);
    return;
  }
  srand(time(NULL));
  set<int> tmp_nodes;
  set<int>::iterator sit;
  while (tmp_nodes.size() < num) {
    int node = rand() % gsize;
    tmp_nodes.insert(node);
  }
  for (sit = tmp_nodes.begin(); sit != tmp_nodes.end(); sit++)
    nodes.push_back(*sit);
}

// default strategy: randomly generate #num_labels DFS labels
void Query::mutipleLabeling(int num_labels) {
  int index = 0;
  vector<pair<int, int>> dfslabels;
  graillabels = vector<vector<pair<int, int>>>(gsize, vector<pair<int, int>>());
  for (int i = 0; i < num_labels; i++) {
    dfslabels.clear();
    //	GraphUtil::randomDFSLabeling(gategraph,dfslabels);
    GraphUtil::grail_labeling(g, dfslabels);
    index = 0;
    for (int j = 0; j < gsize; j++) {
      if (gates->get(j)) {
        graillabels[j].push_back(dfslabels[index]);
        index++;
      }
    }
  }
  useMultiLabels = true;
}

void Query::computeTopoOrder() {
  useTopoOrder = true;
  GraphUtil::topological_sort(g, topoid);
}

/**
 * @brief Check if vertex src can reach vertex trg in the graph.
 *
 * This is the main query interface. The actual implementation depends on
 * the method_name setting:
 * - DFS: Simple depth-first search
 * - GATEDFS: Gate-based DFS using local gates
 * - Index-based: Uses precomputed lin/lout labels
 *
 * @param src Source vertex ID
 * @param trg Target vertex ID
 * @return true if src can reach trg, false otherwise
 */
bool Query::reach(int src, int trg) {
  return GraphUtil::DFSReachCnt(g, src, trg, visited, QueryCnt);
  //	return GraphUtil::DFSReachVisitedNum(g,src,trg,visitnum);
}

bool Query::reachWithoutMat(int src, int trg) {
  return GraphUtil::DFSReachCnt(g, src, trg, visited, QueryCnt);
  //	return GraphUtil::DFSReachVisitedNum(g,src,trg,visitnum);
}

bool Query::test_reach(int src, int trg) {
  bool r = reach(src, trg);
  bool ans = GraphUtil::DFSReach(g, src, trg);
  if (r != ans) {
    cout << "###Wrong: [" << src << "] to [" << trg << "] reach = " << r
         << '\n';
    cout << "----------------------------------------------------" << '\n';
    displayLabelsByNode(src, cout);
    displayLabelsByNode(trg, cout);
    cout << "----------------------------------------------------" << '\n';
    //		exit(0);
  }
  return true;
}

bool Query::test_nomreach(int src, int trg) {
  bool r = reachWithoutMat(src, trg);
  bool ans = GraphUtil::DFSReach(g, src, trg);
  if (r != ans) {
    cout << "###Wrong: [" << src << "] to [" << trg << "] reach = " << r
         << '\n';
    cout << "----------------------------------------------------" << '\n';
    displayLabelsByNode(src, cout);
    displayLabelsByNode(trg, cout);
    cout << "----------------------------------------------------" << '\n';
    exit(0);
  }
  return true;
}

/**
 * @brief Initialize materialization for query acceleration.
 *
 * Materialization precomputes reachability information for high-degree vertices
 * (hubs). This significantly speeds up queries involving these vertices by:
 * 1. Precomputing in-neighbors within epsilon distance for hub vertices
 * 2. Precomputing local gates (incoming/outgoing) for all vertices
 *
 * The number of materialized vertices is adaptive:
 * - Default: 0.1% of vertices (minimum 100, maximum 10 for very large graphs)
 * - Selection: Based on in-degree (highest degree vertices are materialized)
 */
void Query::initMaterialization() {
  inoutgates =
      vector<vector<vector<int>>>(gsize, vector<vector<int>>(2, vector<int>()));
  inneigs = vector<bit_vector *>(gsize, NULL);
  int pnum = (int)(gsize * 0.001);
  pnum = max(pnum, 100);
  if (gatesize > 1500000)
    pnum = min(pnum, 10);
  pnum = min(gsize, pnum);
  cout << "Materialize local gates pnum=" << pnum << '\n';
  materialization(pnum);
  ismaterialized = true;
}

void Query::materialization(int num) {
  cout << "Materialize top 0.1% highest indegree vertices" << '\n';
  selectMaterialized(num);
  //	cout << "Precompute inneighbors" << '\n';
  for (int i = 0; i < gsize; i++) {
    if (materialized->get(i))
      materializeInNeighbors(i); // collect paritial inneighbors and ingates
  }
  cout << "Precompute incoming local gates" << '\n';
  for (int i = 0; i < gsize; i++) {
    if (gates->get(i))
      continue;
    precomputeGates(i, false); // collect ingates
  }
  cout << "Precompute outgoing local gates" << '\n';
  for (int i = 0; i < gsize; i++) {
    if (gates->get(i))
      continue;
    precomputeGates(i, true); // collect outgates
  }
  useLocalGates = true;

// for test
#ifdef PATHTREE_DEBUG
  displayInfor(cout);
#endif
}

void Query::selectMaterialized(int num) {
  num = min(gsize, num);
  multimap<int, int> indegmap;
  int indeg;
  for (int i = 0; i < gsize; i++) {
    indeg = g.in_degree(i);
    indegmap.insert(make_pair(indeg, i));
  }
  multimap<int, int>::reverse_iterator mit = indegmap.rbegin();
  for (int i = 0; i < num; i++) {
    materialized->set_one(mit->second);
    mit++;
  }
}

void Query::materializeInNeighbors(int vid) {
  inneigs[vid] = new bit_vector(gsize);
  QueryCnt++;
  int u, val, index = 0, endindex = 0, nid;
  EdgeList el;
  EdgeList::iterator eit;
  ref += radius + 1;
  que[0] = vid;
  dist[vid] = ref;
  endindex = 1;
  index = 0;
  while (index < endindex) {
    u = que[index];
    index++;
    val = dist[u];
    el = g.in_edges(u);
    for (eit = el.begin(); eit != el.end(); eit++) {
      nid = (*eit);
      if (dist[nid] < ref) {
        dist[nid] = val + 1;
        inneigs[vid]->set_one(nid);
        /*
        if (!gates->get(vid)&&gates->get(nid))
                inoutgates[vid][0].push_back(nid);
        */
        if (val + 1 - ref < radius)
          que[endindex++] = nid;
      }
    }
  }
}

/**
 * @brief Precompute local gates for a vertex within epsilon distance.
 *
 * Local gates are gate vertices reachable from (or reaching) a vertex within
 * the epsilon radius. This enables efficient query answering by:
 * 1. Finding gates reachable from source (out=true)
 * 2. Finding gates that can reach target (out=false)
 *
 * Algorithm: BFS traversal up to epsilon steps, collecting gate vertices.
 *
 * Note: For scalable gate graph generation:
 *   - Outgoing local gates: gates within epsilon steps
 *   - Incoming local gates: gates within epsilon steps (symmetric)
 *
 * @param vid Vertex ID to compute local gates for
 * @param out If true, compute outgoing gates; if false, compute incoming gates
 */
void Query::precomputeGates(int vid, bool out) {
  if (gates->get(vid)) {
    return;
  }
  int u, val, index = 0, endindex = 0, nid;
  EdgeList el;
  EdgeList::iterator eit;
  ref += radius + 2;
  que[0] = vid;
  dist[vid] = ref;
  endindex = 1;
  index = 0;
  while (index < endindex) {
    u = que[index];
    index++;
    val = dist[u];
    if (out)
      el = g.out_edges(u);
    else
      el = g.in_edges(u);
    for (eit = el.begin(); eit != el.end(); eit++) {
      nid = (*eit);
      if (dist[nid] < ref) {
        dist[nid] = val + 1;
        if (gates->get(nid)) {
          if (out)
            inoutgates[vid][1].push_back(nid);
          else
            inoutgates[vid][0].push_back(nid);
          continue;
        }
        if (out) {
          if (val + 1 - ref < radius)
            que[endindex++] = nid;
        } else {
          //	if (val+1-ref<radius+1)
          if (val + 1 - ref < radius)
            que[endindex++] = nid;
        }
      }
    }
  }
}

void Query::displayInfor(ostream &out) {
  for (int i = 0; i < gsize; i++)
    displayInfor(i, out);
}

void Query::displayInfor(int vid, ostream &out) {
  if (materialized->get(vid)) {
    if (inneigs[vid] == NULL) {
      cout << "Error: " << vid << " is NULL" << '\n';
      return;
    }
    out << "Inneigs[" << vid << "]: ";
    for (int i = 0; i < gsize; i++)
      if (inneigs[vid]->get(i))
        cout << i << " ";
    out << '\n';
  }
  out << "InOutGates[" << vid << "]: ";
  vector<int>::iterator vit;
  for (vit = inoutgates[vid][0].begin(); vit != inoutgates[vid][0].end(); vit++)
    out << *vit << " ";
  out << " | ";
  for (vit = inoutgates[vid][1].begin(); vit != inoutgates[vid][1].end(); vit++)
    out << *vit << " ";
  out << "#" << '\n';
}

void Query::displayIndex(ostream &out) {
  if (indextype == 3)
    return;
  out << "Lin and Lout Index " << '\n';
  for (int i = 0; i < gsize; i++) {
    out << i << ": ";
    if (indextype != 1) {
      for (int j = 0; j < lin[i].size(); j++)
        out << lin[i][j] << " ";
    }
    out << "# ";
    if (indextype != 0) {
      for (int j = 0; j < lout[i].size(); j++)
        out << lout[i][j] << " ";
    }
    out << "#" << '\n';
  }
}

void Query::displayLabelsByNode(int vid, ostream &out) {
  if (method_name == "GATEDFS") {
    displayLocalGatesByNode(vid, cout);
    return;
  }
  out << vid << ": [";
  for (int j = 0; j < labels[vid].size() - 1; j++)
    out << labels[vid][j] << " ";
  out << labels[vid][labels[vid].size() - 1] << "]" << '\n';
}

void Query::displayLabels(ostream &out) {
  if (gates->num_bits_set() == 0)
    return;
  out << "Node Labels (only for gate vertices)" << '\n';
  for (int i = 0; i < gsize; i++) {
    if (gates->get(i)) {
      displayLabelsByNode(i, out);
    }
  }
}

void Query::displayGates(ostream &out) {
  out << "Gates: ";
  for (int i = 0; i < gsize; i++) {
    if (gates->get(i)) {
      out << i << " ";
      if (i % 20 == 0)
        out << '\n';
    }
  }
  out << "#" << '\n';
}

void Query::displayGrailLabels(ostream &out) {
  if (!useMultiLabels)
    return;
  out << "GRAIL Labels" << '\n';
  for (int i = 0; i < gsize; i++) {
    if (gates->get(i)) {
      out << i << ": ";
      for (int j = 0; j < graillabels[i].size(); j++)
        out << "[" << graillabels[i][j].first << "," << graillabels[i][j].second
            << "] ";
      out << '\n';
    }
  }
}

void Query::displayLocalGatesByNode(int vid, ostream &out) {
  if (!useLocalGates && !usePartialLocalGates)
    return;
  cout << vid << " ";
  if (gates->get(vid))
    cout << "is gate node" << '\n';
  else
    cout << "is not gate node" << '\n';
  out << "Local Gates[";
  out << vid << "]: ";
  for (int j = 0; j < inoutgates[vid][0].size(); j++)
    out << inoutgates[vid][0][j] << " ";
  out << "# ";
  for (int j = 0; j < inoutgates[vid][1].size(); j++)
    out << inoutgates[vid][1][j] << " ";
  out << "#" << '\n';
}

void Query::displayLocalGates(ostream &out) {
  if (!useLocalGates && !usePartialLocalGates)
    return;
  out << "Local Gates" << '\n';
  for (int i = 0; i < gsize; i++) {
    displayLocalGatesByNode(i, out);
  }
}
