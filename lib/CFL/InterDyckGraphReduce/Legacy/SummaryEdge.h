#ifndef SUMMARY_EDGE_H
#define SUMMARY_EDGE_H
#include <algorithm>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define MEM_USAGE()                                                            \
  {                                                                            \
    string line;                                                               \
    ifstream in("/proc/self/status");                                          \
    for (unsigned i = 0; i < 16; i++) {                                        \
      getline(in, line);                                                       \
    }                                                                          \
    istringstream inl(line);                                                   \
    string x;                                                                  \
    unsigned mem;                                                              \
    inl >> x >> mem;                                                           \
    cout << "mem  = " << (double)mem / 1024 << "M\n";                          \
    in.close();                                                                \
  }

using namespace std;

struct NodewithEID {
  unsigned nodeID;
  // edge id is the kind of edges
  unsigned edgeID;
  NodewithEID(unsigned n, unsigned e) {
    nodeID = n;
    edgeID = e;
  }
};

class SummaryNode {
public:
  unsigned id;
  // red edge is bracket edge
  // array index is the edge parens id
  vector<NodewithEID> inRedEdgeNodes;
  vector<NodewithEID> outRedEdgeNodes;
  // blue edge is parenthesis edge
  vector<NodewithEID> inBlueEdgeNodes;
  vector<NodewithEID> outBlueEdgeNodes;

  // unordered_set<unsigned> rIncanExEid;
  // unordered_set<unsigned> rOutcanExEid;
  // unordered_set<unsigned> bIncanExEid;
  // unordered_set<unsigned> bOutcanExEid;
  unsigned belongto;

  //// merge will only perform, when this is set to be false;
  // bool merged;

  SummaryNode(unsigned i, unsigned eidNum) {
    id = i;
    belongto = i;
    // merged = false;
  }
};

// typedef unordered_set<string> CompleteSummaryEdgeSet;

class SummaryGraph {

private:
  //// -1 not sure, 0 false, 1 true
  // int** rcanExtendMem;
  // int** bcanExtendMem;
public:
  unordered_map<unsigned, unsigned> node2orignodeid;
  unordered_map<unsigned, string> eid2origeidstring;
  vector<SummaryNode *> nodes;
  // input file name
  string filename;
  // the following set contain complete summary edges, i.e. it contains all
  // amount of certain type of summary edge
  //    CompleteSummaryEdgeSet cset;

  stringstream convert;

  // TODO can have two numbers, one for red, one for blue.
  // eidNum is number of edge labels + 1, the one extra represent not clear what
  // label it is for the summary edge.
  unsigned eidNum;

  SummaryGraph(const string &f) {
    filename = f;
    constructGraph();
  }

  ~SummaryGraph() {
    for (SummaryNode *node : nodes) {
      delete node;
    }
  }

  SummaryGraph(const SummaryGraph &) = delete;
  SummaryGraph &operator=(const SummaryGraph &) = delete;

  unordered_map<unsigned, string>
  getEidToOrig(unordered_map<string, unsigned> &str2eid) {
    unordered_map<unsigned, string> result;
    for (unordered_map<string, unsigned>::iterator it = str2eid.begin();
         it != str2eid.end(); it++) {
      result[it->second] = it->first;
    }
    return result;
  }

  void constructGraph() {
    unordered_map<string, unsigned> string2nodeid = readNodes();
    int vtxNum = string2nodeid.size();
    // cout << "Node Number: " << vtxNum << '\n';
    nodes.reserve(vtxNum);
    unordered_map<string, unsigned> str2eid = getEdgeIDs();
    eid2origeidstring = getEidToOrig(str2eid);
    eidNum = str2eid.size() + 1;
    for (int i = 0; i < vtxNum; i++) {
      nodes.push_back(new SummaryNode(i, str2eid.size()));
    }
    readEdges(string2nodeid, str2eid);
  }

  string getEdgeLabel(const string &l) {
    size_t b, e;
    b = l.find_first_of("\"");
    e = l.find_last_of("\"");

    return l.substr(b + 1, (e - b - 1));
  }

  /***
   *
   * whether the line of edge represent an open parenthesis/bracket
   */
  bool isOpen(string edgeStringLabel) {
    return edgeStringLabel.find("o") != string::npos;
  }

  /***
if* return node pair in the string of an edge in the input file
   * node pair returned in string format.
   */
  pair<string, string> getNodePair(string line) {
    string delimiter = "->";
    string str = line.substr(0, line.find_first_of("["));
    string::size_type delimiterstart = str.find_first_of(delimiter);
    string from, to;
    from = str.substr(0, delimiterstart);
    to = str.substr(delimiterstart + delimiter.size());
    return make_pair(from, to);
  }

  unordered_map<string, unsigned> getEdgeIDs() {
    unordered_map<string, unsigned> result;
    unsigned eid = 0;

    string line;
    ifstream in(filename.c_str(), ifstream::in);

    while (getline(in, line)) {
      if (!isEdge(line)) {
        continue;
      }

      string edgeLabel = getEdgeLabel(line);
      if (edgeLabel == "normal") {
        continue;
      }
      edgeLabel = edgeLabel.substr(1);
      // cout << "edge label is " << edgeLabel << '\n';
      if (result.find(edgeLabel) == result.end()) {
        result[edgeLabel] = eid;
        eid++;
      }
    }
    return result;
  }

  unordered_map<string, unsigned> readNodes() {
    unordered_map<string, unsigned> result;
    unsigned nid = 0;

    string line;
    ifstream in(filename.c_str(), ifstream::in);

    while (getline(in, line)) {
      if (!isEdge(line)) {
        continue;
      }
      string from, to;
      pair<string, string> nodesstr = getNodePair(line);
      from = nodesstr.first;
      to = nodesstr.second;

      if (result.find(from) == result.end()) {
        result[from] = nid;
        unsigned orignodeid;
        stringstream nodestr(from);
        nodestr >> orignodeid;
        node2orignodeid[nid] = orignodeid;
        // cout << "node " << nid << " has original id " << orignodeid << " == "
        // << from << '\n';
        nid++;
      }

      if (result.find(to) == result.end()) {
        result[to] = nid;
        unsigned orignodeid;
        stringstream nodestr(to);
        nodestr >> orignodeid;
        node2orignodeid[nid] = orignodeid;
        // cout << "node " << nid << " has original id " << orignodeid << " == "
        // << to << '\n';
        nid++;
      }
    }
    return result;
  }

  /***
   *
   * check whether edge string is a bracket
   */
  bool isBracket(string edgeStringLabel) {
    return edgeStringLabel.find("b") != string::npos;
  }

  bool isParenthesis(string edgeStringLabel) {
    return edgeStringLabel.find("p") != string::npos;
  }

  // read from dot file with specific format.
  void readEdges(unordered_map<string, unsigned> &str2nid,
                 unordered_map<string, unsigned> &str2eid) {
    string line;
    ifstream in(filename.c_str(), ifstream::in);
    while (getline(in, line)) {
      if (!isEdge(line)) {
        continue;
      }
      string from, to;
      pair<string, string> nodes = getNodePair(line);
      string edgeLabel = getEdgeLabel(line);

      // Neutral edges are preserved in the original graph by graph_simp.py,
      // but they do not participate in either colored Dyck projection.
      if (edgeLabel == "normal") {
        continue;
      }

      if (isOpen(edgeLabel)) {
        from = nodes.first;
        to = nodes.second;
      } else {
        from = nodes.second;
        to = nodes.first;
      }
      edgeLabel = edgeLabel.substr(1);

      unsigned fromid = str2nid[from];
      unsigned toid = str2nid[to];

      if (isBracket(edgeLabel)) {
        insertBracketEdge(fromid, toid, str2eid[edgeLabel]);
      } else if (isParenthesis(edgeLabel)) {
        insertParenthesisEdge(fromid, toid, str2eid[edgeLabel]);
      } else {
        cout << "Error, the edge is neither bracket nor parenthesis.\n";
        exit(1);
      }
    }
  }

  /***
   *
   * insert bracket edge
   */
  void insertBracketEdge(unsigned fid, unsigned tid, unsigned eid) {
    // cout << "Edge: " << fid << " to " << tid << " color: red.\n";
    NodewithEID eidToNode(tid, eid);
    NodewithEID eidFromNode(fid, eid);
    nodes[fid]->outRedEdgeNodes.push_back(eidToNode);
    nodes[tid]->inRedEdgeNodes.push_back(eidFromNode);
  }

  /***
   *  insert parenthesis edge
   */
  void insertParenthesisEdge(unsigned fid, unsigned tid, unsigned eid) {
    // cout << "Edge: " << fid << " to " << tid << " color: blue.\n";
    NodewithEID eidToNode(tid, eid);
    NodewithEID eidFromNode(fid, eid);
    nodes[fid]->outBlueEdgeNodes.push_back(eidToNode);
    nodes[tid]->inBlueEdgeNodes.push_back(eidFromNode);
  }

  /***
   * judge whether the input line from input file represents an edge
   */
  bool isEdge(const string &line) {
    if (line.find("->") == string::npos)
      return false;
    else
      return true;
  }
};

unsigned root(unsigned id, SummaryGraph &sGraph) {
  SummaryNode *node = sGraph.nodes[id];
  if (node->id != node->belongto) {
    node->belongto = root(sGraph.nodes[node->belongto]->id, sGraph);
  }
  return node->belongto;
  // postcondition: root(node).belongto == node
  // node.belongto = root(node)
}

#endif
