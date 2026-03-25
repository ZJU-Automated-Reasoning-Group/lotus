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

#ifndef DYCKAA_DYCKGRAPHNODE_H
#define DYCKAA_DYCKGRAPHNODE_H

#include <map>
#include <set>

class DyckGraph;

class DyckGraphNode {
  friend class DyckGraph;

private:
  static int GlobalNodeIndex;
  int NodeIndex;
  const char *NodeName;
  bool ContainsNull = false;

  std::set<void *> InLables;
  std::set<void *> OutLables;

  std::map<void *, std::set<DyckGraphNode *>> InNodes;
  std::map<void *, std::set<DyckGraphNode *>> OutNodes;

  /// only store non-null value
  std::set<void *> EquivClass;

  /// The constructor is not visible. The first argument is the pointer of the
  /// value that you want to encapsulate. The second argument is the name of the
  /// vertex, which will be used in void DyckGraph::printAsDot() function.
  /// please use DyckGraph::retrieveDyckVertex for initialization.
  explicit DyckGraphNode(void *V, const char *Name = nullptr);

public:
  ~DyckGraphNode();

  /// Get its index
  /// The index of the first vertex you create is 0, the second one is 1, ...
  int getIndex() const;

  /// Get its name
  const char *getName();

  /// Get the source vertices corresponding the label
  std::set<DyckGraphNode *> *getInVertices(void *Label);

  /// Get the target vertices corresponding the label
  std::set<DyckGraphNode *> *getOutVertices(void *Label);

  /// Get a single source vertex corresponding the label
  /// if there are multiple such vertices or zero, return null
  DyckGraphNode *getInVertex(void *Label);

  /// Get a single target vertex corresponding the label
  /// if there are multiple such vertices or zero, return null
  DyckGraphNode *getOutVertex(void *Label);

  /// Get the number of vertices that are the targets of this vertex, and have
  /// the edge label: label.
  unsigned int outNumVertices(void *Label);

  /// Get the number of vertices that are the sources of this vertex, and have
  /// the edge label: label.
  unsigned int inNumVertices(void *Label);

  /// Total degree of the vertex
  unsigned int degree();

  /// Get all the labels in the edges that point to the vertex's targets.
  std::set<void *> &getOutLabels();

  /// Get all the labels in the edges that point to the vertex.
  std::set<void *> &getInLabels();

  /// Get all the vertex's targets.
  /// The return value is a map which maps labels to a set of vertices.
  std::map<void *, std::set<DyckGraphNode *>> &getOutVertices();

  /// Get all the vertex's sources.
  /// The return value is a map which maps labels to a set of vertices.
  std::map<void *, std::set<DyckGraphNode *>> &getInVertices();

  /// Add a target with a label. Meanwhile, this vertex will be a source of ver.
  void addTarget(DyckGraphNode *Node, void *Label);

  /// Remove a target. Meanwhile, this vertex will be removed from ver's sources
  void removeTarget(DyckGraphNode *Node, void *Label);

  /// Return true if the vertex contains a target ver, and the edge label is
  /// "label"
  bool containsTarget(DyckGraphNode *Tar, void *Label);

  /// For qirun's algorithm DyckGraph::qirunAlgorithm().
  /// The representatives of all the vertices in the equivalent set of this
  /// vertex will be set to be Rep.
  void mvEquivalentSetTo(DyckGraphNode *RootRep);

  /// Get the equivalent set of non-null value.
  /// Use it after you call DyckGraph::qirunAlgorithm().
  std::set<void *> *getEquivalentSet();

  /// the equivalent set contains null pointer
  void setContainsNull() { ContainsNull = true; }

  /// return true if the equivalent set contains null pointer
  bool containsNull() const { return ContainsNull; }

private:
  void addSource(DyckGraphNode *, void *Label);

  void removeSource(DyckGraphNode *, void *Label);
};

#endif // DYCKAA_DYCKGRAPHNODE_H
