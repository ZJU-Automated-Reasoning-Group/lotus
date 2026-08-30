#include "Legacy/SummaryEdge.h"
#include "Utils/ADT/UnionFind.h"

#include <iostream>
#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace std;
string merge_orig_to_colormerge = "tmp_merge_orig_to_color_merge.txt";

void findMatch(unsigned nodeID, SummaryGraph &sGraph, unsigned eid,
               unordered_set<unsigned> &usedNodes, unsigned origFrom,
               unsigned origTo) {
  SummaryNode *node = sGraph.nodes[nodeID];
  if (usedNodes.find(nodeID) != usedNodes.end()) {
    return;
  }
  usedNodes.insert(nodeID);
  for (vector<NodewithEID>::iterator it = node->inBlueEdgeNodes.begin();
       it != node->inBlueEdgeNodes.end(); it++) {
    unsigned cureid = it->edgeID;
    unsigned nextnid = it->nodeID;
    if (eid == cureid && nodeID != origTo && nextnid != origFrom) {
      cout << "    match " << nodeID << " <-" << cureid << "- " << nextnid;
    }
  }
  for (vector<NodewithEID>::iterator it = node->inRedEdgeNodes.begin();
       it != node->inRedEdgeNodes.end(); it++) {
    unsigned nextnid = it->nodeID;
    findMatch(nextnid, sGraph, eid, usedNodes, origFrom, origTo);
  }
  for (vector<NodewithEID>::iterator it = node->outRedEdgeNodes.begin();
       it != node->outRedEdgeNodes.end(); it++) {
    unsigned nextnid = it->nodeID;
    findMatch(nextnid, sGraph, eid, usedNodes, origFrom, origTo);
  }

  usedNodes.erase(nodeID);
}

void findMatch(unsigned nodeID, SummaryGraph &sGraph) {
  SummaryNode *node = sGraph.nodes[nodeID];
  unordered_set<unsigned> usedNodes;
  // usedNodes.insert(nodeID);
  for (vector<NodewithEID>::iterator it = node->outBlueEdgeNodes.begin();
       it != node->outBlueEdgeNodes.end(); it++) {
    unsigned eid = it->edgeID;
    unsigned nextnid = it->nodeID;
    cout << "Candidate: " << nodeID << " -" << eid << "-> " << nextnid;
    findMatch(nextnid, sGraph, eid, usedNodes, nodeID, nextnid);
    cout << '\n';
  }
}

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
}

void printGraphBlue(SummaryGraph &sGraph) {
  unordered_map<unsigned, unsigned> node2orig = sGraph.node2orignodeid;
  unordered_map<unsigned, string> eid2orig = sGraph.eid2origeidstring;
  for (unsigned i = 0; i < sGraph.nodes.size(); i++) {
    SummaryNode *node = sGraph.nodes[i];
    if (node->belongto != node->id)
      continue;
    for (vector<NodewithEID>::iterator it = node->outBlueEdgeNodes.begin();
         it != node->outBlueEdgeNodes.end(); it++) {
      unsigned tonodeID = root(it->nodeID, sGraph);
      unsigned eid = it->edgeID;
      cout << node2orig[i] << "->" << node2orig[tonodeID] << "[label=\"o"
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

void exportMergeNodes(SummaryGraph &sGraph) {
  unordered_map<unsigned, list<unsigned> *> mergemap;
  for (unsigned i = 0; i < sGraph.nodes.size(); i++) {
    SummaryNode *node = sGraph.nodes[i];
    unsigned root = node->belongto;
    if (mergemap.find(root) == mergemap.end()) {
      mergemap[root] = new list<unsigned>();
    }
    mergemap[root]->push_back(i);
  }

  ofstream out(merge_orig_to_colormerge);
  for (unordered_map<unsigned, list<unsigned> *>::iterator it =
           mergemap.begin();
       it != mergemap.end(); it++) {
    list<unsigned> *lst = it->second;
    unsigned root = it->first;
    out << root << ": ";
    for (list<unsigned>::iterator lit = lst->begin(); lit != lst->end();
         lit++) {
      out << *lit << " ";
    }
    out << '\n';
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    cerr << "usage: lotus-cfl-inter-dyck-graphaux <graph.dot>\n";
    return 1;
  }
  SummaryGraph sGraph(argv[1]);
  queue<pair<unsigned, unsigned>> worklist;
  for (unsigned i = 0; i < sGraph.nodes.size(); i++) {
    // findMatch(i, sGraph);
    SummaryNode *node = sGraph.nodes[i];
    for (vector<NodewithEID>::iterator it = node->outRedEdgeNodes.begin();
         it != node->outRedEdgeNodes.end(); it++) {
      unsigned tonodeID = it->nodeID;
      worklist.push(make_pair(i, tonodeID));
    }
  }

  UnionFind m_sets(sGraph.nodes.size());

  while (!worklist.empty()) {
    unsigned first = worklist.front().first;
    unsigned second = worklist.front().second;
    m_sets.join(first, second);
    worklist.pop();
  }

  unordered_map<unsigned, list<unsigned> *> nodeGroups;
  for (unsigned i = 0; i < sGraph.nodes.size(); i++) {
    unsigned root = m_sets.find(i);
    if (nodeGroups.find(root) == nodeGroups.end()) {
      nodeGroups[root] = new list<unsigned>();
    }
    nodeGroups[root]->push_back(i);
  }

  for (unordered_map<unsigned, list<unsigned> *>::iterator ngit =
           nodeGroups.begin();
       ngit != nodeGroups.end(); ngit++) {
    unsigned cur, prev;
    list<unsigned> *lst = ngit->second;
    for (list<unsigned>::iterator lit = lst->begin(); lit != lst->end();
         lit++) {
      // cout << "list item: " << *lit;
      cur = *lit;
      if (lit == lst->begin()) {
        prev = cur;
        continue;
      }
      mergeNodes(prev, cur, sGraph);
      // cout << "merge " << prev << " with " << cur << '\n';
      //  last line
      prev = cur;
    }
    cout << "\n\n";
  }
  printGraphBlue(sGraph);
  createNodeMergeMap(sGraph);
  exportMergeNodes(sGraph);
  return 0;
}
