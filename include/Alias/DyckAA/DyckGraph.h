/*
 *  Canary features a fast unification-based alias analysis for C programs
 *  Copyright (C) 2021 Qingkai Shi <qingkaishi@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef DYCKAA_DYCKHALFGRAPH_H
#define DYCKAA_DYCKHALFGRAPH_H

#include "Alias/DyckAA/DyckGraphNode.h"

#include <stack>
#include <unordered_map>

class DyckGraphEdgeLabel;

/// This class models a dyck-cfl language as a graph, which does not contain the
/// barred edges. See details in
/// http://dl.acm.org/citation.cfm?id=2491956.2462159&coll=DL&dl=ACM&CFID=379446910&CFTOKEN=65130716
/// .
class DyckGraph {
private:
  std::set<DyckGraphNode *> Vertices;

  std::unordered_map<void *, DyckGraphNode *> ValVertexMap;

  /// edge labels
  /// @{
  DyckGraphEdgeLabel *DerefEdgeLabel;
  std::map<long, DyckGraphEdgeLabel *> OffsetEdgeLabelMap;
  std::map<long, DyckGraphEdgeLabel *> IndexEdgeLabelMap;
  /// @}

public:
  DyckGraph();

  ~DyckGraph();

  /// The number of vertices in the graph.
  unsigned int numVertices();

  /// The number of equivalent sets.
  /// Please use it after you call void qirunAlgorithm().
  unsigned int numEquivalentClasses();

  /// Get the set of vertices in the graph.
  std::set<DyckGraphNode *> &getVertices();

  /// You are not recommended to use the function when the graph is big,
  /// because it is time-consuming.
  void printAsDot(const char *FileName) const;

  /// Combine x's rep and y's rep.
  DyckGraphNode *combine(DyckGraphNode *NodeX, DyckGraphNode *NodeY);

  /// if value is NULL, a new vertex will be always returned with false.
  /// if value's vertex has been initialized, it will be returned with true;
  /// otherwise, it will be initialized and returned with false;
  /// If a new vertex is initialized, it will be added into the graph.
  std::pair<DyckGraphNode *, bool>
  retrieveDyckVertex(void *Val, const char *Name = nullptr);

  DyckGraphNode *findDyckVertex(void *Val);

  /// Get reachable nodes
  /// @{
  void getReachableVertices(const std::set<DyckGraphNode *> &Sources,
                            std::set<DyckGraphNode *> &Reachable);

  void getReachableVertices(DyckGraphNode *Source,
                            std::set<DyckGraphNode *> &Reachable);
  /// @}

  /// The algorithm proposed by Qirun Zhang.
  /// Find the paper here:
  /// http://dl.acm.org/citation.cfm?id=2491956.2462159&coll=DL&dl=ACM&CFID=379446910&CFTOKEN=65130716
  /// . Note that if there are two edges with the same label: a->b and a->c, b
  /// and c will be put into the same equivelant class. If the function does
  /// nothing, return true, otherwise return false.
  bool qirunAlgorithm();

  /// validation
  void validation(const char *, int);

  DyckGraphEdgeLabel *getOrInsertOffsetEdgeLabel(long Offset);

  DyckGraphEdgeLabel *getOrInsertIndexEdgeLabel(long Offset);

  DyckGraphEdgeLabel *getDereferenceEdgeLabel() const { return DerefEdgeLabel; }

private:
  void removeFromWorkList(std::multimap<DyckGraphNode *, void *> &,
                          DyckGraphNode *, void *);

  bool containsInWorkList(std::multimap<DyckGraphNode *, void *> &,
                          DyckGraphNode *, void *);
};

#endif // DYCKAA_DYCKHALFGRAPH_H
