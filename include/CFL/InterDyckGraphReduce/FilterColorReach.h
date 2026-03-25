#ifndef filterColorReach_h
#define filterColorReach_h

#include "CFL/InterDyckGraphReduce/SummaryEdge.h"
#include <iostream>
#include <list>
#include <string>
#include <unordered_map>
//#include <unordered_set>

using namespace std;
string merge_orig_to_colormerge = "tmp_merge_orig_to_color_merge.txt";

string colorreach_graph = "tmp_color_reach_dk_graph.dot";

// only merge out edges
// b edges merged into a
// Warning: resulting graph can have duplicate edges
void mergeNodes(unsigned anodeID, unsigned bnodeID, SummaryGraph &sGraph) {
  if (root(anodeID, sGraph) != anodeID || root(bnodeID, sGraph) != bnodeID)
    return mergeNodes(root(anodeID, sGraph), root(bnodeID, sGraph), sGraph);

  if (anodeID == bnodeID)
    return;

  SummaryNode *anode = sGraph.nodes[anodeID];
  SummaryNode *bnode = sGraph.nodes[bnodeID];

  bnode->belongto = anode->id;

  for (vector<NodewithEID>::iterator it = bnode->outRedEdgeNodes.begin();
       it != bnode->outRedEdgeNodes.end(); it++) {
    unsigned eid = it->edgeID;
    unsigned tonodeID = root(it->nodeID, sGraph);
    NodewithEID newEidNode(tonodeID, eid);
    // do not care duplicate
    anode->outRedEdgeNodes.push_back(newEidNode);
    // cout << "add new red edge " << anode->id << "->" << tonodeID << " with
    // eid " << eid << '\n';
  }
  bnode->outRedEdgeNodes.clear();

  for (vector<NodewithEID>::iterator it = bnode->outBlueEdgeNodes.begin();
       it != bnode->outBlueEdgeNodes.end(); it++) {
    unsigned eid = it->edgeID;
    unsigned tonodeID = root(it->nodeID, sGraph);
    NodewithEID newEidNode(tonodeID, eid);
    // do not care duplicate
    anode->outBlueEdgeNodes.push_back(newEidNode);
    // cout << "add new blue edge " << anode->id << "->" << tonodeID << " with
    // eid " << eid << '\n';
  }
  bnode->outBlueEdgeNodes.clear();

  // cout << "Merge " << anodeID << " with " << bnodeID << '\n';
  if (anodeID == 72 && bnodeID == 75) {
    unsigned a = 1;
    a++;
  }
}

void printGraphBlue(SummaryGraph &sGraph) {
  unordered_map<unsigned, unsigned> node2orig = sGraph.node2orignodeid;
  unordered_map<unsigned, string> eid2orig = sGraph.eid2origeidstring;
  ofstream out(colorreach_graph);
  for (unsigned i = 0; i < sGraph.nodes.size(); i++) {
    SummaryNode *node = sGraph.nodes[i];
    if (node->belongto != node->id)
      continue;
    for (vector<NodewithEID>::iterator it = node->outBlueEdgeNodes.begin();
         it != node->outBlueEdgeNodes.end(); it++) {
      unsigned tonodeID = root(it->nodeID, sGraph);
      unsigned eid = it->edgeID;
      out << node2orig[i] << "->" << node2orig[tonodeID] << "[label=\"o"
          << eid2orig[eid] << "\"]" << '\n';
    }
  }
}

void createNodeMergeMap(SummaryGraph &sGraph) {
  unordered_map<unsigned, unsigned> node2orig = sGraph.node2orignodeid;
  unordered_map<unsigned, string> eid2orig = sGraph.eid2origeidstring;
  ofstream out("tmp_color_reach_dk_node_merge.txt", ofstream::out);
  unordered_map<unsigned, list<unsigned> *> mergeMap;
  for (unsigned i = 0; i < sGraph.nodes.size(); i++) {
    SummaryNode *node = sGraph.nodes[i];
    if (node->id != root(node->id, sGraph)) {
      unsigned existID = node->belongto;
      if (mergeMap.find(existID) == mergeMap.end()) {
        mergeMap[existID] = new list<unsigned>();
      }
      mergeMap[existID]->push_back(node->id);
    } else {
      unsigned id = node->id;
      if (mergeMap.find(id) == mergeMap.end()) {
        mergeMap[id] = new list<unsigned>();
      }
      mergeMap[id]->push_back(id);
    }
  }

  for (unordered_map<unsigned, list<unsigned> *>::iterator mit =
           mergeMap.begin();
       mit != mergeMap.end(); mit++) {
    unsigned existID = mit->first;
    list<unsigned> *l = mit->second;
    out << node2orig[existID] << ": ";
    for (list<unsigned>::iterator lit = l->begin(); lit != l->end(); lit++) {
      out << node2orig[*lit] << " ";
    }
    out << '\n';
  }
}

unordered_map<unsigned, list<unsigned> *> *
exportMergeNodes(SummaryGraph &sGraph) {
  unordered_map<unsigned, list<unsigned> *> *mergemap =
      new unordered_map<unsigned, list<unsigned> *>();
  for (unsigned i = 0; i < sGraph.nodes.size(); i++) {
    SummaryNode *node = sGraph.nodes[i];
    unsigned root = node->belongto;
    if (mergemap->find(root) == mergemap->end()) {
      (*mergemap)[root] = new list<unsigned>();
    }
    (*mergemap)[root]->push_back(i);
  }

  ofstream out(merge_orig_to_colormerge);
  for (unordered_map<unsigned, list<unsigned> *>::iterator it =
           mergemap->begin();
       it != mergemap->end(); it++) {
    list<unsigned> *lst = it->second;
    unsigned root = it->first;
    out << root << ": ";
    for (list<unsigned>::iterator lit = lst->begin(); lit != lst->end();
         lit++) {
      out << *lit << " ";
    }
    out << '\n';
  }
  return mergemap;
}

SummaryGraph *colorreach_main() {
  string line;
  ifstream in("dotfile/exp-2020");
  getline(in, line);
  SummaryGraph *sGraph = new SummaryGraph(line);
  queue<pair<unsigned, unsigned>> worklist;
  for (unsigned i = 0; i < sGraph->nodes.size(); i++) {
    SummaryNode *node = sGraph->nodes[i];
    for (vector<NodewithEID>::iterator it = node->outRedEdgeNodes.begin();
         it != node->outRedEdgeNodes.end(); it++) {
      unsigned tonodeID = it->nodeID;
      worklist.push(make_pair(i, tonodeID));
    }
  }

  while (!worklist.empty()) {
    unsigned first = worklist.front().first;
    unsigned second = worklist.front().second;
    mergeNodes(first, second, *sGraph);
    worklist.pop();
  }
  printGraphBlue(*sGraph);
  createNodeMergeMap(*sGraph);
  exportMergeNodes(*sGraph);

  return sGraph;
}
#endif
