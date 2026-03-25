#ifndef CFLREACH_H
#define CFLREACH_H

#include "CFL/InterDyckGraphReduce/CFLGraph.h"
#include <algorithm>
//#include <bitset>
#include <fstream>
#include <iostream>
//#include <iterator>
//#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std;

// enum FromOrTo {f, t};
// enum edgeTy {D, A};
// enum EdgeTy {non, EdgeTyD, EdgeTyA, M, DV, V, MAM, M_q, MA_s, MA, AM_s, AM,
// Ab, Db, e, numEdgeTy}; //note non means no edge, e means empty edge.

unordered_map<string, EdgeTy> EdgeMap;
unsigned S_edge = 0;

ifstream in("dotfile/exp-2020");

// typedef unsigned int       u32;

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

class ReachabilityItem {
public:
  ReachabilityItem(unsigned a, unsigned b, unsigned c)
      : vtx1(a), vtx2(b), edgeTy(c) {};
  unsigned GetEdgeTy() const { return edgeTy; }
  unsigned GetVtx1() const { return vtx1; }
  unsigned GetVtx2() const { return vtx2; }
  void SetEdgeTy(unsigned edg1) { edgeTy = edg1; }
  void SetVtx1(unsigned a) { vtx1 = a; }
  void SetVtx2(unsigned b) { vtx2 = b; }

private:
  unsigned vtx1;
  unsigned vtx2;
  unsigned edgeTy;
};

class CFLGrammar {
public:
  CFLGrammar(unsigned a, unsigned b, unsigned c, unsigned ep)
      : LTerm(a), RTerm1(b), RTerm2(c), epsilon(ep) {
    termNum = 3;
  }
  CFLGrammar(unsigned a, unsigned b, unsigned ep)
      : LTerm(a), RTerm1(b), epsilon(ep) {
    termNum = 2;
  }
  CFLGrammar() {};
  int GetTermNum() { return termNum; }
  int IsEpsilonRule() {
    if (termNum == 2 && RTerm1 == epsilon)
      return 1;
    else
      return 0;
  }
  int IsSingleRule() {
    if (termNum == 2)
      return 1;
    else
      return 0;
  }
  unsigned GetLTerm() { return LTerm; }
  unsigned GetRTerm1() { return RTerm1; }
  unsigned GetRTerm2() { return RTerm2; }
  void SetLTerm(unsigned a) {
    LTerm = a;
    termNum++;
  }
  void SetRTerm1(unsigned a) {
    RTerm1 = a;
    termNum++;
  }
  void SetRTerm2(unsigned a) {
    RTerm2 = a;
    termNum++;
  }
  void PrintGrammar() {
    cout << LTerm << " -> " << RTerm1;
    if (termNum == 3)
      cout << " " << RTerm2;
    cout << '\n';
  }

private:
  int termNum;
  unsigned LTerm;
  unsigned RTerm1;
  unsigned RTerm2;
  unsigned epsilon;
};

class SimpleDotParser {
public:
  SimpleDotParser() {};
  pair<string, string> ReturnNodePair(string &src, const string &delimiter);

  unsigned BuildNodeMap(const string &infile,
                        unordered_map<string, unsigned> &NodeID);

  void BuildHashTable(const string &infile,
                      unordered_map<string, unsigned> &NodeID,
                      unordered_map<string, unsigned> &EdgeID,
                      vector<CFLGrammar> &CGVec, CFLHashMap &cm);
  void BuildMyHashTable(const string &infile,
                        unordered_map<string, unsigned> &NodeID,
                        unordered_map<string, unsigned> &EdgeID,
                        CFLHashMap &cm);

  void StripExtra(string &line) {
    line = line.substr(0, line.find_first_of("["));
  }
  int IsEdge(const string &line) {
    if (line.find("->") == string::npos)
      return 0;
    else
      return 1;
  }

  string GetEdgeLabel(const string &line) {
    size_t b, e;
    b = line.find_first_of("\"");
    e = line.find_last_of("\"");

    return line.substr(b + 1, (e - b - 1));
  }
};

pair<string, string> SimpleDotParser::ReturnNodePair(string &src,
                                                     const string &delimiter) {
  string from, to;

  StripExtra(src);
  string::size_type delimiterstart = src.find_first_of(delimiter);
  //  istringstream fromstr(src.substr(0, delimiterstart));
  // istringstream tostr(src.substr(delimiterstart + delimiter.size()));

  from = src.substr(0, delimiterstart);
  to = src.substr(delimiterstart + delimiter.size());

  // fromstr>>from;
  // tostr>>to;

  return make_pair(from, to);

  //  string::size_type found = 0, start = 0;
}

unsigned SimpleDotParser::BuildNodeMap(
    const string &infile,
    unordered_map<string, unsigned> &NodeID) { // return node num.
  string line;
  ifstream in(infile.c_str());

  while (getline(in, line)) {
    if (IsEdge(line)) {

      string from, to;
      // EdgeTy edgTy = GetEdgeTy(line);
      // cout<<"line "<< line<< "of type"<<GetEdgeTy(line)<<'\n';

      pair<string, string> nodes = ReturnNodePair(line, "->");
      from = nodes.first;
      to = nodes.second;
      // cout<<"from "<<from<<" to "<<to<<'\n';

      if (NodeID.find(from) == NodeID.end()) { // can't find from
        unsigned id = NodeID.size();
        // cout<<from<<" should assign "<<id<<'\n';
        NodeID[from] = id;
      }

      if (NodeID.find(to) == NodeID.end()) { // can't find to
        unsigned id = NodeID.size();
        // cout<<to<<" should assign "<<id<<'\n';
        NodeID[to] = id;
      }
    }
  }

  return NodeID.size();
}

void SimpleDotParser::BuildMyHashTable(const string &infile,
                                       unordered_map<string, unsigned> &NodeID,
                                       unordered_map<string, unsigned> &EdgeID,
                                       CFLHashMap &cm) {

  string line;

  ifstream in(infile.c_str());

  // edgeTy edg;
  while (getline(in, line)) {
    // cout<<line<< "haha  " <<'\n';
    if (IsEdge(line)) {

      string from, to;
      // EdgeTy edgTy = GetEdgeTy(line);
      string edgelabel = GetEdgeLabel(line);
      // int togrammar=0;

      // build edge map
      if (EdgeID.find(edgelabel) == EdgeID.end()) {
        unsigned id = EdgeID.size();
        EdgeID[edgelabel] = id;
        // togrammar=1;
      }

      // Matrix *q;
      // cout<<"line "<< line<< "of type"<<GetEdgeTy(line)<<'\n';
      // cout<<"label "<<edgelabel<<" hash "<<EdgeID[edgelabel]<<'\n';
      pair<string, string> nodes = ReturnNodePair(line, "->");
      from = nodes.first;
      to = nodes.second;
      cm.InsertEdge(NodeID[from], NodeID[to], EdgeID[edgelabel]);
    }
  }
}

void SimpleDotParser::BuildHashTable(const string &infile,
                                     unordered_map<string, unsigned> &NodeID,
                                     unordered_map<string, unsigned> &EdgeID,
                                     vector<CFLGrammar> &CGVec,
                                     CFLHashMap &cm) {

  string line;

  ifstream in(infile.c_str());

  // map S and e to edgeid
  if (EdgeID.find("S") == EdgeID.end()) {
    unsigned id = EdgeID.size();
    EdgeID["S"] = id;
  }

  S_edge = EdgeID["S"];
  if (EdgeID.find("e") == EdgeID.end()) {
    unsigned id = EdgeID.size();
    EdgeID["e"] = id;
  }

  // insert S-> SS  S-> e
  CFLGrammar cg1(EdgeID["S"], EdgeID["S"], EdgeID["S"], EdgeID["e"]);
  CGVec.push_back(cg1);
  CFLGrammar cg2(EdgeID["S"], EdgeID["e"], EdgeID["e"]);
  CGVec.push_back(cg2);

  // edgeTy edg;
  while (getline(in, line)) {
    // cout<<line<< "haha  " <<'\n';
    if (IsEdge(line)) {

      string from, to;
      // EdgeTy edgTy = GetEdgeTy(line);
      string edgelabel = GetEdgeLabel(line);
      int togrammar = 0;

      // build edge map
      if (EdgeID.find(edgelabel) == EdgeID.end()) {
        unsigned id = EdgeID.size();
        EdgeID[edgelabel] = id;
        togrammar = 1;
      }

      // Matrix *q;
      // cout<<"line "<< line<< "of type"<<GetEdgeTy(line)<<'\n';
      // cout<<"label "<<edgelabel<<" hash "<<EdgeID[edgelabel]<<'\n';
      pair<string, string> nodes = ReturnNodePair(line, "->");
      from = nodes.first;
      to = nodes.second;
      cm.InsertEdge(NodeID[from], NodeID[to], EdgeID[edgelabel]);

      // add the reverse edge

      string edgelabel_ab = edgelabel + "ab";
      if (EdgeID.find(edgelabel_ab) == EdgeID.end()) {
        unsigned id = EdgeID.size();
        EdgeID[edgelabel_ab] = id;
      }
      cm.InsertEdge(NodeID[to], NodeID[from], EdgeID[edgelabel_ab]);

      string edgelabel_A = edgelabel + "A";
      if (EdgeID.find(edgelabel_A) == EdgeID.end()) {
        unsigned id = EdgeID.size();
        EdgeID[edgelabel_A] = id;
      }

      if (togrammar) {
        // we have S -> a S ab;
        // will be transfer to
        // S-> A ab;
        // A-> a S;

        CFLGrammar cg1(EdgeID["S"], EdgeID[edgelabel_A], EdgeID[edgelabel_ab],
                       EdgeID["e"]);
        CGVec.push_back(cg1);
        CFLGrammar cg2(EdgeID[edgelabel_A], EdgeID[edgelabel], EdgeID["S"],
                       EdgeID["e"]);
        CGVec.push_back(cg2);
      }
    }
  }
}

#endif
