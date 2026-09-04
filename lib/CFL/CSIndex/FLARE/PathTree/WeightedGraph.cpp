#include "CFL/CSIndex/FLARE/PathTree/WeightedGraph.h"

#include <algorithm>

namespace lotus::cfl::cs_index::flare::path_tree {

/**
 * @brief Default constructor.
 *
 * Initializes an empty directed weighted graph with no vertices or edges.
 */
WeightedGraph::WeightedGraph() {
  graph = DWGRA();
  vl = DWVertexList();
  maxEdgeId = 0;
}

/**
 * @brief Constructor with existing graph data.
 * @param g Graph adjacency structure
 * @param vlist Vertex list
 *
 * Creates a directed weighted graph from existing adjacency structure and
 * vertex list.
 */
WeightedGraph::WeightedGraph(DWGRA &g, DWVertexList &vlist) {
  graph = g;
  vl = vlist;
}

/**
 * @brief Constructor that reads graph from input stream.
 * @param in Input stream containing graph data
 *
 * Creates a directed weighted graph by reading data from the provided input
 * stream.
 */
WeightedGraph::WeightedGraph(istream &in) { readGraph1(in); }

/**
 * @brief Destructor.
 *
 * Cleans up resources when the graph is destroyed.
 */
WeightedGraph::~WeightedGraph() {}

/**
 * @brief Print graph structure to standard output.
 *
 * Delegates to writeGraph with cout as the output stream.
 */
void WeightedGraph::printGraph() { writeGraph(cout); }

/**
 * @brief Clear all vertices and edges from the graph.
 *
 * Resets the graph to an empty state with no vertices or edges.
 */
void WeightedGraph::clear() {
  maxEdgeId = 0;
  graph.clear();
  vl.clear();
  edgeOpMap.clear();
}

/**
 * @brief Trim whitespace from string.
 * @param str String to trim
 *
 * Removes trailing whitespace characters from the string.
 */
void WeightedGraph::strTrim(string &str) {
  string whitespaces(" \t");
  int index = str.find_last_not_of(whitespaces);
  if (index != string::npos)
    str.erase(index + 1);
  else
    str.clear();
}

void WeightedGraph::readGraph(istream &in) {
  string buf;
  getline(in, buf);

  strTrim(buf);
  if (buf != "graph_for_greach") {
    cout << "BAD FILE FORMAT!" << "\n";
    exit(0);
  }

  int n;
  getline(in, buf);
  istringstream(buf) >> n;
  for (int i = 0; i < n; i++)
    addVertex(i);

  string sub;
  int idx;
  int sid = 0;
  int tid = 0;
  while (getline(in, buf)) {
    strTrim(buf);
    idx = buf.find(":");
    buf.erase(0, idx + 2);
    while (buf.find(" ") != string::npos) {
      sub = buf.substr(0, buf.find(" "));
      istringstream(sub) >> tid;
      buf.erase(0, buf.find(" ") + 1);
      addEdgeWithWeight(sid, tid, 0);
    }
    ++sid;
  }
}

void WeightedGraph::readGraph1(istream &in) {
  string buf;
  getline(in, buf);

  strTrim(buf);
  if (buf != "graph_for_greach") {
    cout << "BAD FILE FORMAT!" << "\n";
    //	exit(0);
  }

  int n;
  getline(in, buf);
  istringstream(buf) >> n;
  for (int i = 0; i < n; i++)
    addVertex(i);

  string sub;
  int idx;
  int sid = 0;
  int tid = 0;
  int eid, weight;
  maxEdgeId = 0;
  while (getline(in, buf)) {
    strTrim(buf);
    idx = buf.find(":");
    buf.erase(0, idx + 2);
    while (buf.find(" ") != string::npos) {
      sub = buf.substr(0, buf.find(" "));
      string idstr = sub.substr(0, sub.find("["));
      istringstream(idstr) >> tid;
      sub.erase(0, sub.find("[") + 1);
      string eidstr = sub.substr(0, sub.find("|"));
      istringstream(eidstr) >> eid;
      sub.erase(0, sub.find("|") + 1);
      //	string wstr = sub.substr(0,sub.find("|"));
      string wstr = sub.substr(0, sub.find("]"));
      istringstream(wstr) >> weight;
      buf.erase(0, buf.find(" ") + 1);
      addEdge(sid, tid, weight, eid);
      if (eid > maxEdgeId)
        maxEdgeId = eid;
    }
    ++sid;
  }
}

void WeightedGraph::writeGraph(ostream &out) {
  out << "graph_for_greach" << "\n";
  out << vl.size() << "\n";

  DWGRA::iterator git;
  DWEdgeList el;
  DWEdgeList::iterator eit;
  DWVertexList::iterator vit;
  for (vit = vl.begin(); vit != vl.end(); vit++) {
    out << vit->first << ": ";
    if (graph.find(vit->first) != graph.end()) {
      el = graph[vit->first].outList;
      for (eit = el.begin(); eit != el.end(); eit++) {
        if (edgeOpMap.find(*eit) == edgeOpMap.end()) {
          cerr << "output error: edgeid " << *eit
               << " is not existed in edgeOpMap!" << "\n";
          exit(1);
          continue;
        }
        out << edgeOpMap[*eit].trg << "[" << *eit << "|"
            << edgeOpMap[*eit].weight << "] ";
      }
    } else {
      cerr << "output error: " << vit->first << " is not existed!" << "\n";
      exit(1);
    }
    out << "#" << "\n";
  }
}

void WeightedGraph::toGDL(ostream &out) {
  out << "graph: {color: aquamarine\n"
      << "\t\tamax\t\t\t: 160\n"
      << "\t\tscaling\t\t\t: maxspect\n"
      << "\t\tarrowmode\t\t: free\n"
      << "\t\tedge.arrowsize\t: 4\n"
      << "\t\tsmanhattanedges\t: no\n"
      << "\t\tmanhattanedges\t: no\n"
      << "\t\tsplines\t\t: no\n\n";
  DWVertexList::iterator vit;
  DWEdgeOpMap::iterator eit;
  for (eit = edgeOpMap.begin(); eit != edgeOpMap.end(); eit++) {
    out << "edge: { sourcename: \"" << eit->second.src << "\""
        << " targetname: \"" << eit->second.trg << "\" }" << "\n";
  }
  out << "\n";

  for (vit = vl.begin(); vit != vl.end(); vit++) {
    out << "node: { title: \"" << vit->first << "\"}" << "\n";
  }
  out << "\n";
  out << "}" << "\n";
}

void WeightedGraph::addVertex(int vid) {
  DWVertex v;
  v.id = vid;
  v.visited = false;

  vl[vid] = v;
  DWEdgeList el = graph[vid].outList;
}

void WeightedGraph::removeVertex(int vid) {
  // remove all edges connected with vertex
  DWEdgeList el = graph[vid].inList;
  DWEdgeList::iterator eit;
  for (eit = el.begin(); eit != el.end(); eit++) {
    // Dec 15
    // change *eit to edgeOpMap[*eit].src
    removeEdge(edgeOpMap[*eit].src, vid);
  }
  el = graph[vid].outList;
  for (eit = el.begin(); eit != el.end(); eit++) {
    // Dec 15
    // change *eit to edgeOpMap[*eit].trg
    removeEdge(vid, edgeOpMap[*eit].trg);
  }
  // remove vertex
  vl.erase(vid);
  graph.erase(vid);
}

void WeightedGraph::removeVertexfromVL(int vid) { vl.erase(vid); }

void WeightedGraph::addEdge(int sid, int tid, int weight, int edgeid) {
  if (vl.find(sid) == vl.end())
    addVertex(sid);
  if (vl.find(tid) == vl.end())
    addVertex(tid);

  int osid = -1;
  int otid = -1;
  // if found
  if (edgeOpMap.find(edgeid) != edgeOpMap.end()) {
    osid = edgeOpMap[edgeid].src;
    otid = edgeOpMap[edgeid].trg;
  }

  edgeOpMap[edgeid].src = sid;
  edgeOpMap[edgeid].trg = tid;
  edgeOpMap[edgeid].weight = weight;

  // update out edge list
  if (sid != osid)
    graph[sid].outList.push_back(edgeid);
  // update in edge list
  if (tid != otid)
    graph[tid].inList.push_back(edgeid);

  if (sid != osid && tid != otid)
    maxEdgeId++;
}

void WeightedGraph::addEdgeWithWeight(int sid, int tid, int weight) {
  if (vl.find(sid) == vl.end())
    addVertex(sid);
  if (vl.find(tid) == vl.end())
    addVertex(tid);

  maxEdgeId++;
  edgeOpMap[maxEdgeId].src = sid;
  edgeOpMap[maxEdgeId].trg = tid;
  edgeOpMap[maxEdgeId].weight = weight;

  // update out edge list
  graph[sid].outList.push_back(maxEdgeId);
  // update in edge list
  graph[tid].inList.push_back(maxEdgeId);
}

void WeightedGraph::updateEdge(int sid, int tid, int weight) {
  if (vl.find(sid) == vl.end())
    addVertex(sid);
  if (vl.find(tid) == vl.end())
    addVertex(tid);

  int edgeid = -1;
  DWEdgeList el = graph[sid].outList;
  DWEdgeList::iterator eit;
  for (eit = el.begin(); eit != el.end(); eit++)
    if (edgeOpMap[*eit].trg == tid) {
      edgeid = *eit;
      break;
    }

  if (edgeid == -1) {
    maxEdgeId++;
    edgeOpMap[maxEdgeId].src = sid;
    edgeOpMap[maxEdgeId].trg = tid;
    edgeOpMap[maxEdgeId].weight = weight;
    graph[sid].outList.push_back(maxEdgeId);
    graph[tid].inList.push_back(maxEdgeId);
  } else {
    edgeOpMap[edgeid].src = sid;
    edgeOpMap[edgeid].trg = tid;
    edgeOpMap[edgeid].weight = weight;
  }
}

void WeightedGraph::removeEdge(int sid, int tid) {
  if (vl.find(sid) == vl.end()) {
    //	cout << "Src [" << sid << "] is not existed!" << "\n";
    return;
  }
  if (vl.find(tid) == vl.end()) {
    //	cout << "Trg [" << tid << "] is not existed!" << "\n";
    return;
  }
  vector<int> id_list;
  vector<int>::iterator vit;
  DWEdgeOpMap::iterator emit;
  for (emit = edgeOpMap.begin(); emit != edgeOpMap.end(); emit++) {
    if (emit->second.src == sid && emit->second.trg == tid) {
      id_list.push_back(emit->first);
    }
  }
  for (vit = id_list.begin(); vit != id_list.end(); vit++) {
    edgeOpMap.erase(*vit);
    // Dec 15
    // update edge list
    graph[sid].outList.remove(*vit);
    graph[tid].inList.remove(*vit);
  }

  /*
  DWEdgeList::iterator eit;
  for (eit = graph[sid].outList.begin(); eit != graph[sid].outList.end(); ) {
          if (find(id_list.begin(), id_list.end(), *eit) != id_list.end()) {
                  graph[sid].outList.erase(eit++);
          }
          else
                  ++eit;
  }
  for (eit = graph[tid].inList.begin(); eit != graph[tid].inList.end(); ) {
          if (find(id_list.begin(), id_list.end(), *eit) != id_list.end())
                  graph[tid].inList.erase(eit++);
          else
                  ++eit;
  }
  */
}

void WeightedGraph::removeEdge(int eid) {
  if (edgeOpMap.find(eid) == edgeOpMap.end()) {
    cerr << "Error: edgeid " << eid << " is not existed!" << "\n";
    exit(0);
    return;
  }

  graph[edgeOpMap[eid].src].outList.remove(eid);
  graph[edgeOpMap[eid].trg].inList.remove(eid);
  edgeOpMap.erase(eid);
}

void WeightedGraph::removeEdgeWithID(int sid, int tid, int edgeid) {
  if (vl.find(sid) == vl.end()) {
    cout << "Src [" << sid << "] is not existed!" << "\n";
    return;
  }
  if (vl.find(tid) == vl.end()) {
    cout << "Trg [" << tid << "] is not existed!" << "\n";
    return;
  }

  edgeOpMap.erase(edgeid);

  // Dec 15
  //  update edgelist by list function: remove
  graph[sid].outList.remove(edgeid);
  graph[tid].inList.remove(edgeid);

  /*
  DWEdgeList::iterator eit;
  for (eit = graph[sid].outList.begin(); eit != graph[sid].outList.end(); eit++)
          if (*eit == edgeid) {
                  graph[sid].outList.erase(eit);
                  break;
          }
  for (eit = graph[tid].inList.begin(); eit != graph[tid].inList.end(); eit++)
          if (*eit == edgeid) {
                  graph[tid].inList.erase(eit);
                  break;
          }
  */
}

void WeightedGraph::removeEdgeWithWeight(int sid, int tid, int weight) {
  if (vl.find(sid) == vl.end()) {
    cout << "Src [" << sid << "] is not existed!" << "\n";
    return;
  }
  if (vl.find(tid) == vl.end()) {
    cout << "Trg [" << tid << "] is not existed!" << "\n";
    return;
  }

  int delEdgeId = -1;
  DWEdgeOpMap::iterator emit;
  for (emit = edgeOpMap.begin(); emit != edgeOpMap.end(); emit++) {
    if (emit->second.src == sid && emit->second.trg == tid &&
        emit->second.weight == weight) {
      delEdgeId = emit->first;
      edgeOpMap.erase(delEdgeId);
      break;
    }
  }

  if (delEdgeId == -1)
    return;

  DWEdgeList::iterator eit;
  for (eit = graph[sid].outList.begin(); eit != graph[sid].outList.end(); eit++)
    if (*eit == delEdgeId) {
      graph[sid].outList.erase(eit);
      break;
    }

  for (eit = graph[tid].inList.begin(); eit != graph[tid].inList.end(); eit++)
    if (delEdgeId == *eit) {
      graph[tid].inList.erase(eit);
      break;
    }
}

int WeightedGraph::num_vertices() { return vl.size(); }

int WeightedGraph::num_edges() {
  int num = edgeOpMap.size();
  return num;
}

int WeightedGraph::maxid() {
  int maxid = -1;
  DWVertexList::iterator vlit;
  for (vlit = vl.begin(); vlit != vl.end(); vlit++)
    if (vlit->first > maxid)
      maxid = vlit->first;
  return maxid;
}

// return out edges of specified vertex
DWEdgeList &WeightedGraph::out_edges(int src) { return graph[src].outList; }

// return in edges of specified vertex
DWEdgeList &WeightedGraph::in_edges(int trg) { return graph[trg].inList; }

int WeightedGraph::out_degree(int src) { return graph[src].outList.size(); }

int WeightedGraph::in_degree(int trg) { return graph[trg].inList.size(); }

int WeightedGraph::weight(int src, int trg) {
  int w = 0;
  DWEdgeList el = graph[src].outList;
  DWEdgeList::iterator eit;
  for (eit = el.begin(); eit != el.end(); eit++)
    if (edgeOpMap[*eit].trg == trg) {
      w = edgeOpMap[*eit].weight;
      break;
    }
  return w;
}

int WeightedGraph::edgeId(int src, int trg) {
  int eid = -1;
  DWEdgeOpMap::iterator emit;
  for (emit = edgeOpMap.begin(); emit != edgeOpMap.end(); emit++) {
    if (emit->second.src == src && emit->second.trg == trg) {
      return emit->first;
    }
  }

  return eid;
}

DWVertexProp WeightedGraph::edge(int src, int trg) {
  int edgeid = edgeId(src, trg);
  DWVertexProp ep;
  if (edgeid == -1) {
    ep.id = -1;
    return ep;
  }
  ep.id = trg;
  ep.weight = edgeOpMap[edgeid].weight;
  ep.edgeid = edgeid;
  return ep;
}

// get roots of graph (root is zero in_degree vertex)
set<int> WeightedGraph::getRoots() {
  set<int> roots;
  DWGRA::iterator git;
  for (git = graph.begin(); git != graph.end(); git++)
    if ((*git).second.inList.size() == 0)
      roots.insert((*git).first);

  return roots;
}

// check whether the edge from src to trg is in the graph
bool WeightedGraph::hasEdgeWithID(int src, int trg, int edgeid) {
  if (graph.find(src) == graph.end()) {
    //	cout << "Source vertex [" << src << "] is not existed!" << '\n';
    return false;
  }

  if (edgeOpMap.find(edgeid) != edgeOpMap.end())
    return true;

  return false;
}

bool WeightedGraph::hasEdge(int src, int trg) {
  if (graph.find(src) == graph.end())
    return false;

  DWEdgeOpMap::iterator emit;
  for (emit = edgeOpMap.begin(); emit != edgeOpMap.end(); emit++) {
    if (emit->second.src == src && emit->second.trg == trg)
      return true;
  }

  return false;
}

bool WeightedGraph::hasVertex(int vid) {
  if (vl.find(vid) == vl.end())
    return false;
  return true;
}

// return vertex list of graph
DWVertexList &WeightedGraph::vertices() { return vl; }

WeightedGraph &WeightedGraph::operator=(const WeightedGraph &g) {
  if (this != &g) {
    graph = g.graph;
    vl = g.vl;
    edgeOpMap = g.edgeOpMap;
  }
  return *this;
}

// get a specified vertex property
DWVertex &WeightedGraph::operator[](const int edgeid) {
  int trg = edgeOpMap[edgeid].trg;
  return vl[trg];
}

// Dec 15
int WeightedGraph::weight(int eid) {
  if (edgeOpMap.find(eid) == edgeOpMap.end()) {
    //		cerr << "WARNING on weight: eid " << eid << " is not existed!"
    //<< '\n'; 		if(edgeOpMap.count(eid) > 0){ 			cerr << eid << '\n';
    //		}
    //		throw out_of_range("Error");
    return edgeOpMap[eid].weight;
    exit(-1);
    return MAX_VAL;
  }
  return edgeOpMap[eid].weight;
}

int WeightedGraph::source(int eid) {
  if (edgeOpMap.find(eid) == edgeOpMap.end()) {
    cerr << "Error on source: eid " << eid << " is not existed!" << "\n";
    exit(0);
    return MAX_VAL;
  }
  return edgeOpMap[eid].src;
}

int WeightedGraph::target(int eid) {
  if (edgeOpMap.find(eid) == edgeOpMap.end()) {
    cerr << "Error on target: eid " << eid << " is not existed!" << "\n";
    exit(0);
    return MAX_VAL;
  }
  return edgeOpMap[eid].trg;
}

} // namespace lotus::cfl::cs_index::flare::path_tree
