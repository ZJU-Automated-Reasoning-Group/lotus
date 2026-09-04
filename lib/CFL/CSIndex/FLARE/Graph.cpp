/**
 * @file Graph.cpp
 * @brief Implementation of the Graph class for Context-Sensitive Flow (CFL)
 * reachability analysis.
 *
 * This module implements a graph data structure specifically designed for
 * Extended Dyck-CFL reachability queries in context-sensitive program analysis.
 * The graph supports:
 *
 * - Labeled edges: Positive labels represent call edges, negative labels
 * represent return edges
 * - Summary edge computation: Builds inter-procedural summary edges for
 * efficient reachability
 * - Indexing graph transformation: Converts the graph to a form suitable for
 * indexing
 *
 * The graph is used as the foundation for the CSIndex system, which provides
 * efficient indexing and querying of context-sensitive reachability
 * relationships.
 *
 * Reference: OOPSLA 2022a - "Indexing the Extended Dyck-CFL Reachability for
 * Context-Sensitive Program Analysis" by Qingkai Shi, Yongchao Wang, Peisen
 * Yao, and Charles Zhang.
 */

#include "CFL/CSIndex/FLARE/Graph.h"

#include "Utils/Platform/ProgressBar.h"

#include <algorithm>
#include <cstring>

namespace lotus::cfl::cs_index::flare {

/**
 * @brief Default constructor.
 *
 * Initializes an empty graph with no vertices or edges.
 */
Graph::Graph() {
  graph = GRA();
  vl = VertexList();
}

/**
 * @brief Constructor with initial vertex count.
 * @param size Initial number of vertices
 *
 * Creates a graph with the specified number of vertices and empty edge lists.
 */
Graph::Graph(int size) {
  n_vertices = size;
  vl = VertexList(size);
  graph = GRA(size, In_OutList());
}

/**
 * @brief Constructor with existing graph data.
 * @param g Adjacency list representation
 * @param vlist Vertex list
 *
 * Creates a graph from existing adjacency list and vertex list data.
 */
Graph::Graph(GRA &g, VertexList &vlist) {
  n_vertices = vlist.size();
  graph = g;
  vl = vlist;
}

/**
 * @brief Constructor that reads graph from input stream.
 * @param in Input stream containing graph data
 *
 * Creates a graph by reading data from the provided input stream.
 */
Graph::Graph(istream &in) { readGraph(in); }

/**
 * @brief Destructor.
 *
 * Cleans up resources when the graph is destroyed.
 */
Graph::~Graph() = default;

/**
 * @brief Print graph structure to standard output.
 *
 * Delegates to writeGraph with cout as the output stream.
 */
void Graph::printGraph() { writeGraph(cout); }

/**
 * @brief Clear all vertices and edges from the graph.
 *
 * Resets the graph to an empty state with no vertices or edges.
 */
void Graph::clear() {
  n_vertices = 0;
  graph.clear();
  vl.clear();
}

/**
 * @brief Trim whitespace from the right side of a string.
 * @param str String to trim
 *
 * Removes trailing whitespace characters from the string.
 */
void Graph::strTrimRight(string &str) {
  string whitespaces(" \t");
  int index = str.find_last_not_of(whitespaces);
  if (index != string::npos)
    str.erase(index + 1);
  else
    str.clear();
}

/**
 * @brief Read graph from input stream in the "graph_for_greach" format.
 *
 * Expected format:
 *   - First line: "graph_for_greach"
 *   - Second line: number of vertices
 *   - Subsequent lines: "vertex_id: neighbor1 neighbor2 ... #function_id"
 *
 * Edges can be unlabeled or labeled with context-sensitive IDs:
 *   - Unlabeled: "target_vertex"
 *   - Labeled: "target_vertex.context_id" (positive for calls, negative for
 * returns)
 *
 * @param in Input stream containing the graph data
 */
void Graph::readGraph(istream &in) {
  string buf;
  getline(in, buf);

  strTrimRight(buf);
  if (buf.length() < strlen("graph_for_greach")) {
    cerr << "BAD FILE FORMAT!" << '\n';
    exit(1);
  }

  string tag = buf.substr(0, strlen("graph_for_greach"));
  if (tag != "graph_for_greach") {
    cerr << "BAD FILE FORMAT!" << '\n';
    exit(2);
  }

  int n;
  getline(in, buf);
  istringstream(buf) >> n;
  // initialize
  n_vertices = n;
  vl = VertexList(n);
  graph = GRA(n, In_OutList());

  for (int i = 0; i < n; i++)
    addVertex(i);

  string sub;
  int idx;
  int sid = 0;
  int tid = 0;
  while (getline(in, buf)) {
    strTrimRight(buf);
    idx = buf.find(':');
    buf.erase(0, idx + 2);

    auto startIdx = 0;
    auto emptyIdx = buf.find(' ');
    while (emptyIdx != string::npos) {
      sub = buf.substr(startIdx, emptyIdx - startIdx);

      auto cs_idx = sub.find('.');
      if (cs_idx == string::npos) {
        istringstream(sub) >> tid;
        addEdge(sid, tid);
      } else {
        istringstream(sub.substr(0, cs_idx)) >> tid;
        int cs_id;
        istringstream(sub.substr(cs_idx + 1)) >> cs_id;
        addEdge(sid, tid, cs_id);
      }

      startIdx = emptyIdx + 1;
      emptyIdx = buf.find(' ', startIdx);
    }
    sub = buf.substr(startIdx);
    assert(sub[0] == '#');
    istringstream(sub.substr(1)) >> this->at(sid).func_id;

    ++sid;
  }
}

void Graph::writeGraph(ostream &out) {
  cout << "Graph size = " << graph.size() << '\n';
  out << "graph_for_greach" << '\n';
  out << vl.size() << '\n';

  GRA::iterator git;
  EdgeList el;
  EdgeList::iterator eit;
  for (int i = 0; i < vl.size(); i++) {
    out << i << ": ";
    el = graph[i].outList;
    for (eit = el.begin(); eit != el.end(); eit++)
      out << (*eit) << " ";
    out << "#" << '\n';
  }
}

void Graph::addVertex(int vid) {
  if (vid >= vl.size()) {
    int size = vl.size();
    for (int i = 0; i < (vid - size + 1); i++) {
      graph.push_back(In_OutList());
      vl.push_back(Vertex(vid + i));
    }
    n_vertices = vl.size();
  }

  Vertex v;
  v.id = vid;
  v.top_level = -1;
  v.visited = false;
  vl[vid] = v;

  EdgeList il = EdgeList();
  EdgeList ol = EdgeList();
  In_OutList oil = In_OutList();
  oil.inList = il;
  oil.outList = ol;
  graph[vid] = oil;
}

void Graph::remove_vertex(int vid) {
  cout << vid << '\n';
  EdgeList preds = graph[vid].inList;
  cout << vid << '\n';
  for (const auto &pred_it : preds) {
    cout << pred_it << '\n';
    auto pred = graph[pred_it].outList;
    auto f_it = find(pred.begin(), pred.end(), vid);
    assert(f_it != pred.end());
    pred.erase(f_it);
  }
  EdgeList succs = graph[vid].outList;
  for (const auto &succ_it : succs) {
    auto succ = graph[succ_it].inList;
    auto f_it = find(succ.begin(), succ.end(), vid);
    assert(f_it != succ.end());
    succ.erase(f_it);
  }
  graph[vid].inList.clear();
  graph[vid].outList.clear();
  n_vertices--;
}

void Graph::addEdge(int sid, int tid) {
  if (sid == tid) {
    return;
  }

  if (sid >= vl.size())
    addVertex(sid);
  if (tid >= vl.size())
    addVertex(tid);
  // update edge list
  graph[tid].inList.push_back(sid);
  graph[sid].outList.push_back(tid);
  n_edges++;
}

void Graph::addEdge(int sid, int tid, int label) {
  if (sid >= vl.size())
    addVertex(sid);
  if (tid >= vl.size())
    addVertex(tid);
  // update edge list
  graph[tid].inList.push_back(sid);
  graph[sid].outList.push_back(tid);
  n_edges++;

  assert(label);
  if (label > 0) {
    pos_label_map[std::make_pair(sid, tid)] = label;
  } else {
    neg_label_map[std::make_pair(sid, tid)] = label;
  }
}

int Graph::num_vertices() { return vl.size(); }

int Graph::num_edges() {
  EdgeList el;
  GRA::iterator git;
  int num = 0;
  for (git = graph.begin(); git != graph.end(); git++) {
    el = git->outList;
    num += el.size();
  }
  return num;
}

// return out edges of specified vertex
EdgeList &Graph::out_edges(int src) { return graph[src].outList; }

// return in edges of specified vertex
EdgeList &Graph::in_edges(int trg) { return graph[trg].inList; }

int Graph::out_degree(int src) { return graph[src].outList.size(); }

int Graph::in_degree(int trg) { return graph[trg].inList.size(); }

// get roots of graph (root is zero in_degree vertex)
vector<int> Graph::getRoots() {
  vector<int> roots;
  GRA::iterator git;
  int i = 0;
  for (git = graph.begin(); git != graph.end(); git++, i++) {
    if (git->inList.empty())
      roots.push_back(i);
  }

  return std::move(roots);
}

// check whether the edge from src to trg is in the graph
bool Graph::hasEdge(int src, int trg) {
  EdgeList &el = graph[src].outList;
  return std::any_of(el.begin(), el.end(), [trg](int ei) { return ei == trg; });
}

// return vertex list of graph
VertexList &Graph::vertices() { return vl; }

Graph &Graph::operator=(const Graph &g) {
  if (this != &g) {
    graph = g.graph;
    vl = g.vl;
    n_vertices = g.n_vertices;
  }
  return *this;
}

// get a specified vertex property
Vertex &Graph::operator[](int vid) { return vl[vid]; }

Vertex &Graph::at(int vid) { return vl.at(vid); }

Graph::Graph(unordered_map<int, vector<int>> &inlist,
             unordered_map<int, vector<int>> &outlist) {
  n_vertices = inlist.size();
  cout << "inlist size: " << inlist.size() << '\n';
  cout << "outlist size: " << outlist.size() << '\n';
  vl = VertexList(n_vertices);
  graph = GRA(n_vertices, In_OutList());
  for (int i = 0; i < n_vertices; i++)
    addVertex(i);
  cout << "inlist size: " << inlist.size() << '\n';
  cout << "outlist size: " << outlist.size() << '\n';
  unordered_map<int, vector<int>>::iterator hit, hit1;
  unordered_map<int, int> indexmap;
  vector<int> vec;
  vector<int>::iterator vit;
  int k;
  for (hit = inlist.begin(), k = 0; hit != inlist.end(); hit++, k++) {
    indexmap[hit->first] = k;
  }
  cout << "k: " << k << '\n';
  for (hit = inlist.begin(), hit1 = outlist.begin(), k = 0; hit != inlist.end();
       hit++, hit1++, k++) {
    vec = hit->second;
    for (vit = vec.begin(); vit != vec.end(); vit++)
      graph[k].inList.push_back(indexmap[*vit]);
    vec = hit1->second;
    for (vit = vec.begin(); vit != vec.end(); vit++)
      graph[k].outList.push_back(indexmap[*vit]);
  }
}

void Graph::extract(unordered_map<int, vector<int>> &inlist,
                    unordered_map<int, vector<int>> &outlist) {
  for (int i = 0; i < vl.size(); i++) {
    inlist[i] = graph[i].inList;
    outlist[i] = graph[i].outList;
  }
  //	printMap(inlist,outlist);
}

// for test
void Graph::printMap(unordered_map<int, vector<int>> &inlist,
                     unordered_map<int, vector<int>> &outlist) {
  cout << "==============================================" << '\n';
  unordered_map<int, vector<int>>::iterator hit;
  vector<int>::iterator vit;
  for (hit = outlist.begin(); hit != outlist.end(); hit++) {
    cout << hit->first << ": ";
    vector<int> vec = hit->second;
    for (vit = vec.begin(); vit != vec.end(); vit++)
      cout << *vit << " ";
    cout << "#" << '\n';
  }
  cout << "In List for graph" << '\n';
  for (hit = inlist.begin(); hit != inlist.end(); hit++) {
    cout << hit->first << ": ";
    vector<int> vec = hit->second;
    for (vit = vec.begin(); vit != vec.end(); vit++)
      cout << *vit << " ";
    cout << "#" << '\n';
  }
  cout << "================================================" << '\n';
}

void Graph::print_edges() {
  cout << "----Current Edge sets: ----" << '\n';
  EdgeList el;
  for (int i = 0; i < num_vertices(); i++) {
    el = graph[i].outList;
    for (const auto &e_it : el) {
      cout << i << "->" << e_it << '\n';
    }
  }
  cout << "---------------------------" << '\n';
}

double Graph::tcs(const int vid) { return vl[vid].tcs; }

void Graph::sortEdges() {
  GRA::iterator git;
  for (git = graph.begin(); git != graph.end(); git++) {
    sort(git->inList.begin(), git->inList.end());
    sort(git->outList.begin(), git->outList.end());
  }
}

vector<string> &Graph::split(const string &s, char delim,
                             vector<string> &elems) {
  stringstream ss(s);
  string item;
  while (getline(ss, item, delim)) {
    elems.push_back(item);
  }
  return elems;
}

vector<string> Graph::split(const string &s, char delim) {
  vector<string> elems;
  return split(s, delim, elems);
}

/**
 * @brief Build summary edges for inter-procedural reachability.
 *
 * Summary edges represent reachability across procedure boundaries. This
 * algorithm:
 * 1. Identifies actual-out and formal-out vertices (negative labeled edges)
 * 2. Identifies formal-in vertices (positive labeled edges)
 * 3. Propagates reachability through matching call-return pairs
 *
 * A summary edge (s, t) indicates that vertex s can reach vertex t through
 * a valid inter-procedural path respecting the CFL grammar (matched
 * calls/returns).
 *
 * Algorithm: Worklist-based propagation that matches positive labels with
 * corresponding negative labels to build inter-procedural summary edges.
 */
void Graph::build_summary_edges(bool record_witnesses) {
  std::set<std::pair<int, int>> WorkList;
  std::map<int, std::set<int>> PathEdge;
  std::map<std::pair<int, int>, std::vector<int>> path_witnesses;

  summary_witnesses.clear();

  auto append_path = [](const std::vector<int> &prefix,
                        const std::vector<int> &suffix) {
    std::vector<int> result = prefix;
    if (suffix.empty())
      return result;

    size_t begin = 0;
    if (!result.empty() && result.back() == suffix.front())
      begin = 1;
    result.insert(result.end(), suffix.begin() + begin, suffix.end());
    return result;
  };

  auto propagate = [&PathEdge, &WorkList, &path_witnesses,
                    record_witnesses](int s, int t,
                                      const std::vector<int> &witness) {
    if (!PathEdge[s].count(t)) {
      PathEdge[s].insert(t);
      WorkList.emplace(s, t);
      if (record_witnesses)
        path_witnesses[{s, t}] = witness;
    }
  };

  std::set<int> actualOut, formalin, formalout;
  for (auto &it : neg_label_map) {
    auto &e = it.first;
    PathEdge[e.first].insert(e.first);
    WorkList.emplace(e.first, e.first);
    if (record_witnesses)
      path_witnesses[{e.first, e.first}] = {e.first};
    actualOut.insert(e.second);
    formalout.insert(e.first);
  }
  for (auto &it : pos_label_map) {
    auto &e = it.first;
    formalin.insert(e.second);
  }

  while (!WorkList.empty()) {
    auto it = WorkList.begin();
    auto e = *it; // v->w
    WorkList.erase(it);
    int v = e.first;
    int w = e.second;

    if (actualOut.count(v)) {
      for (auto x : summary_edges[v]) {
        if (record_witnesses) {
          const auto summary_it = summary_witnesses.find({x, v});
          const auto path_it = path_witnesses.find({v, w});
          assert(summary_it != summary_witnesses.end());
          assert(path_it != path_witnesses.end());
          propagate(x, w,
                    append_path(summary_it->second, path_it->second));
        } else {
          propagate(x, w, {});
        }
      }

      for (auto x : graph[v].inList) {
        if (!pos_label_map.count({x, v}) && !neg_label_map.count({x, v})) {
          if (record_witnesses) {
            const auto path_it = path_witnesses.find({v, w});
            assert(path_it != path_witnesses.end());
            std::vector<int> witness = {x};
            witness = append_path(witness, path_it->second);
            propagate(x, w, witness);
          } else {
            propagate(x, w, {});
          }
        }
      }
    } else if (formalin.count(v)) {
      if (formalout.count(w)) {
        for (int vin : graph[v].inList) {
          auto xit = pos_label_map.find({vin, v});
          if (xit == pos_label_map.end())
            continue;
          int x = xit->first.first;
          int label = xit->second;
          for (int wout : graph[w].outList) {
            auto yit = neg_label_map.find({w, wout});
            if (yit == neg_label_map.end())
              continue;
            int y = yit->first.second;
            int label2 = yit->second;
            if (label > 0 && label + label2 == 0) {
              std::vector<int> summary_witness;
              if (record_witnesses) {
                const auto path_it = path_witnesses.find({v, w});
                assert(path_it != path_witnesses.end());
                summary_witness = {x};
                summary_witness =
                    append_path(summary_witness, path_it->second);
                summary_witness.push_back(y);
              }

              if (!hasEdge(x, y)) {
                const bool inserted = summary_edges[y].insert(x).second;
                if (record_witnesses && inserted)
                  summary_witnesses[{x, y}] = summary_witness;
              }
              for (auto a : PathEdge[y]) {
                if (record_witnesses) {
                  const auto path_it = path_witnesses.find({y, a});
                  assert(path_it != path_witnesses.end());
                  propagate(x, a,
                            append_path(summary_witness, path_it->second));
                } else {
                  propagate(x, a, {});
                }
              }
            }
          }
        }
      }
    } else {
      // default
      for (auto x : graph[v].inList) {
        if (!pos_label_map.count({x, v}) && !neg_label_map.count({x, v})) {
          if (record_witnesses) {
            const auto path_it = path_witnesses.find({v, w});
            assert(path_it != path_witnesses.end());
            std::vector<int> witness = {x};
            witness = append_path(witness, path_it->second);
            propagate(x, w, witness);
          } else {
            propagate(x, w, {});
          }
        }
      }
    }
  }
}

/**
 * @brief Transform graph into indexing graph structure.
 *
 * The indexing graph doubles the vertex set to create a bipartite structure:
 * - Original vertices [0, n/2): represent entry points
 * - Duplicate vertices [n/2, n): represent exit points
 *
 * This transformation enables efficient indexing by separating entry and exit
 * contexts. Edges are redirected accordingly:
 * - Internal edges within a function connect original to duplicate vertices
 * - Labeled edges (calls/returns) are removed as they're handled via summary
 * edges
 *
 * The resulting graph structure supports efficient gate-based indexing.
 */
void Graph::to_indexing_graph() {
  // add all summary edges to the graph
  add_summary_edges();

  // Double the vertex set: [0, n/2) for entries, [n/2, n) for exits
  n_vertices = n_vertices * 2;
  vl.resize(n_vertices);
  graph.resize(n_vertices);

  for (int i = num_vertices() / 2; i < num_vertices(); ++i) {
    addVertex(i);
  }
  for (int i = num_vertices() / 2; i < num_vertices(); ++i) {
    int orig_i = i - n_vertices / 2;
    for (int t : out_edges(orig_i)) {
      addEdge(i, t + n_vertices / 2);
    }
    addEdge(orig_i, i);
  }

  for (auto &it : pos_label_map) {
    auto &pos_e = it.first;
    removeEdge(pos_e.first, pos_e.second);
  }
  for (auto &it : neg_label_map) {
    auto &neg_e = it.first;
    removeEdge(neg_e.first + n_vertices / 2, neg_e.second + n_vertices / 2);
  }

  // remove all self-cycles
  for (int i = 0; i < n_vertices; ++i) {
    removeEdge(i, i);
  }
}

void Graph::removeEdge(int s, int t) {
  // update edge list
  auto &inList = graph[t].inList;
  for (int i = 0; i < inList.size(); ++i) {
    if (inList[i] == s) {
      inList[i] = inList.back();
      inList.pop_back();
      --i;
    }
  }

  auto &outList = graph[s].outList;
  for (int i = 0; i < outList.size(); ++i) {
    if (outList[i] == t) {
      outList[i] = outList.back();
      outList.pop_back();
      --i;
      --n_edges;
    }
  }
}

/**
 * @brief Validate graph structure for CFL reachability correctness.
 *
 * Performs several correctness checks:
 * 1. Labeled edges (calls/returns) must be within the same function
 * 2. Formal-in vertices must have positive-labeled incoming edges
 * 3. Reports maximum argument count per function
 *
 * This validation ensures the graph conforms to the expected structure
 * for context-sensitive flow analysis.
 */
void Graph::check() {
  cout << "Checking correctness of the input graph..." << '\n';
  ProgressBar bar("CSIndex graph validation", ProgressBar::PBS_CharacterStyle);
  // Check: labeled edges should not cross function boundaries
  for (int i = 0; i < n_vertices; ++i) {
    Vertex &v = at(i);
    auto &inList = graph[i].inList;
    for (auto t : inList) {
      Vertex &u = at(t);
      if (label(t, i) == 0 && v.func_id != u.func_id) {
        cerr << "invalid graph where a labeled edge is cross funcs" << '\n';
        cerr << t << " -> " << i << "\n";
        exit(8);
      }
    }

    auto &outList = graph[i].outList;
    for (auto t : outList) {
      Vertex &u = at(t);
      if (label(i, t) == 0 && v.func_id != u.func_id) {
        cerr << "invalid graph where a labeled edge is cross funcs" << '\n';
        cerr << i << " -> " << t << "\n";
        exit(18);
      }
    }
    bar.showProgress(static_cast<float>(i + 1) / n_vertices);
  }
  cout << '\n';

  std::map<int, std::set<int>> func_arg_map;
  ProgressBar bar2("CSIndex label validation",
                   ProgressBar::PBS_CharacterStyle);
  std::size_t checked_labels = 0;
  for (auto pit : pos_label_map) {
    int formalin = pit.first.second;
    auto &vertex = this->at(formalin);
    func_arg_map[vertex.func_id].insert(formalin);

    auto &inList = graph[formalin].inList;
    for (auto actualin : inList) {
      if (!pos_label_map.count(std::make_pair(actualin, formalin))) {
        cerr << "actualin -> formalin does not have a positive label" << '\n';
        cerr << actualin << " -> " << formalin << "\n";
        exit(28);
      }
    }
    bar2.showProgress(static_cast<float>(++checked_labels) /
                      pos_label_map.size());
  }

  int max_arg = -1;
  int max_arg_func = -1;
  for (auto &it : func_arg_map) {
    int arg_size = it.second.size();
    if (arg_size > max_arg) {
      max_arg_func = it.first;
      max_arg = it.second.size();
    }
  }
  cout << '\n';
  cout << "# max arg " << max_arg << " in func " << max_arg_func << '\n';
  cout << "Checking done!" << '\n';
}

size_t Graph::summary_edge_size() {
  size_t ret = 0;
  for (auto &it : summary_edges) {
    ret += it.second.size();
  }
  return ret;
}

bool Graph::has_summary_edge(int src, int dst) const {
  const auto it = summary_edges.find(dst);
  return it != summary_edges.end() && it->second.count(src);
}

const vector<int> *Graph::summary_witness(int src, int dst) const {
  const auto it = summary_witnesses.find({src, dst});
  if (it == summary_witnesses.end())
    return nullptr;
  return &it->second;
}

int Graph::label(int s, int t) {
  auto it = pos_label_map.find({s, t});
  if (it != pos_label_map.end()) {
    return it->second;
  }
  it = neg_label_map.find({s, t});
  if (it != neg_label_map.end()) {
    return it->second;
  }
  return 0;
}

void Graph::add_summary_edges() {
  for (auto &sit : summary_edges) {
    auto t = sit.first;
    for (auto s : sit.second) {
      addEdge(s, t);
    }
  }
}

} // namespace lotus::cfl::cs_index::flare
