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

#include "Alias/DyckAA/DyckGraph.h"

#include "Alias/DyckAA/DyckGraphEdgeLabel.h"

#include <cassert>
#include <cstdio>
#include <stack>

DyckGraph::DyckGraph() { DerefEdgeLabel = new DereferenceEdgeLabel; }

DyckGraph::~DyckGraph() {
  for (auto &V : Vertices)
    delete V;

  delete DerefEdgeLabel;
  auto OIt = OffsetEdgeLabelMap.begin();
  while (OIt != OffsetEdgeLabelMap.end()) {
    delete OIt->second;
    OIt++;
  }
  auto IIt = IndexEdgeLabelMap.begin();
  while (IIt != IndexEdgeLabelMap.end()) {
    delete IIt->second;
    IIt++;
  }
}

DyckGraphEdgeLabel *DyckGraph::getOrInsertOffsetEdgeLabel(long Offset) {
  if (OffsetEdgeLabelMap.count(Offset)) {
    return OffsetEdgeLabelMap[Offset];
  } else {
    DyckGraphEdgeLabel *Ret = new PointerOffsetEdgeLabel(Offset);
    OffsetEdgeLabelMap.insert(
        std::pair<long, DyckGraphEdgeLabel *>(Offset, Ret));
    return Ret;
  }
}

DyckGraphEdgeLabel *DyckGraph::getOrInsertIndexEdgeLabel(long Offset) {
  if (IndexEdgeLabelMap.count(Offset)) {
    return IndexEdgeLabelMap[Offset];
  } else {
    DyckGraphEdgeLabel *Ret = new FieldIndexEdgeLabel(Offset);
    IndexEdgeLabelMap.insert(
        std::pair<long, DyckGraphEdgeLabel *>(Offset, Ret));
    return Ret;
  }
}

void DyckGraph::printAsDot(const char *FileName) const {
  FILE *FileDesc = fopen(FileName, "w+");
  fprintf(FileDesc, "digraph ptg {\n");

  auto VIt = Vertices.begin();
  while (VIt != Vertices.end()) {
    if ((*VIt)->getName() != nullptr)
      fprintf(FileDesc, "\ta%d[label=\"%s\"];\n", (*VIt)->getIndex(),
              (*VIt)->getName());
    else
      fprintf(FileDesc, "\ta%d;\n", (*VIt)->getIndex());

    std::map<void *, std::set<DyckGraphNode *>> &Outs =
        (*VIt)->getOutVertices();
    auto OIt = Outs.begin();
    while (OIt != Outs.end()) {
      long Label = (long)(OIt->first);
      std::set<DyckGraphNode *> *Tars = &OIt->second;
      auto TarIt = Tars->begin();
      while (TarIt != Tars->end()) {
        fprintf(FileDesc, "\ta%d->a%d [label=\"%ld\"];\n", (*VIt)->getIndex(),
                (*TarIt)->getIndex(), Label);
        TarIt++;
      }
      OIt++;
    }
    ++VIt;
  }

  fprintf(FileDesc, "}\n");
  fclose(FileDesc);
}

void DyckGraph::removeFromWorkList(std::multimap<DyckGraphNode *, void *> &List,
                                   DyckGraphNode *Node, void *Label) {
  using CIT = std::multimap<DyckGraphNode *, void *>::iterator;
  using Range = std::pair<CIT, CIT>;
  Range NodeRange = List.equal_range(Node);
  auto Next = NodeRange.first;
  while (Next != NodeRange.second) {
    if ((Next)->second == Label) {
      List.erase(Next);
      return;
    }
    Next++;
  }
}

bool DyckGraph::containsInWorkList(std::multimap<DyckGraphNode *, void *> &List,
                                   DyckGraphNode *v, void *l) {
  using CIT = std::multimap<DyckGraphNode *, void *>::iterator;
  using Range = std::pair<CIT, CIT>;
  Range NodeRange = List.equal_range(v);
  auto Next = NodeRange.first;
  while (Next != NodeRange.second) {
    if ((Next)->second == l) {
      return true;
    }
    Next++;
  }
  return false;
}

DyckGraphNode *DyckGraph::combine(DyckGraphNode *NodeX, DyckGraphNode *NodeY) {
  assert(Vertices.count(NodeX));
  assert(Vertices.count(NodeY));
  if (NodeX == NodeY)
    return NodeX;

  if (NodeX->degree() < NodeY->degree()) {
    DyckGraphNode *Temp = NodeX;
    NodeX = NodeY;
    NodeY = Temp;
  }

  std::set<void *> &YOutLabels = NodeY->getOutLabels();
  auto YOIt = YOutLabels.begin();
  while (YOIt != YOutLabels.end()) {
    if (NodeY->containsTarget(NodeY, *YOIt)) {
      if (!NodeX->containsTarget(NodeX, *YOIt)) {
        NodeX->addTarget(NodeX, *YOIt);
      }
      NodeY->removeTarget(NodeY, *YOIt);
    }
    YOIt++;
  }

  YOIt = YOutLabels.begin();
  while (YOIt != YOutLabels.end()) {
    std::set<DyckGraphNode *> *Ws = &NodeY->getOutVertices()[*YOIt];
    auto W = Ws->begin();
    while (W != Ws->end()) {
      if (!NodeX->containsTarget(*W, *YOIt)) {
        NodeX->addTarget(*W, *YOIt);
      }
      // cannot use removeTarget function, which will affect iterator
      // y remove target *w
      DyckGraphNode *WTemp = *W;
      Ws->erase(W++);
      // *w remove src y
      ((WTemp)->getInVertices())[*YOIt].erase(NodeY);
    }
    YOIt++;
  }

  std::set<void *> &YInLabels = NodeY->getInLabels();
  auto YIIt = YInLabels.begin();
  while (YIIt != YInLabels.end()) {
    std::set<DyckGraphNode *> *Ws = &NodeY->getInVertices()[*YIIt];
    auto W = Ws->begin();
    while (W != Ws->end()) {
      if (!(*W)->containsTarget(NodeX, *YIIt)) {
        (*W)->addTarget(NodeX, *YIIt);
      }

      // cannot use removeTarget function, which will affect iterator
      DyckGraphNode *WTemp = *W;
      Ws->erase(W++);
      ((WTemp)->getOutVertices())[*YIIt].erase(NodeY);
    }

    YIIt++;
  }
  auto *Vals = NodeY->getEquivalentSet();
  for (auto &Val : *Vals) {
    ValVertexMap[Val] = NodeX;
  }
  NodeY->mvEquivalentSetTo(NodeX);
  Vertices.erase(NodeY);
  delete NodeY;
  return NodeX;
}

bool DyckGraph::qirunAlgorithm() {
  bool Ret = true;
  std::multimap<DyckGraphNode *, void *> Worklist;
  auto VIt = Vertices.begin();
  while (VIt != Vertices.end()) {
    std::set<void *> &OutLabels = (*VIt)->getOutLabels();
    auto LabelIt = OutLabels.begin();
    while (LabelIt != OutLabels.end()) {
      if ((*VIt)->outNumVertices(*LabelIt) > 1) {
        Worklist.insert(std::pair<DyckGraphNode *, void *>(*VIt, *LabelIt));
      }
      LabelIt++;
    }
    VIt++;
  }

  if (!Worklist.empty())
    Ret = false;

  while (!Worklist.empty()) {
    // outs()<<"HERE0\n"; outs().flush();
    auto ZIt = Worklist.begin();
    // outs()<<"HERE0.1\n"; outs().flush();
    std::set<DyckGraphNode *> *Nodes =
        &ZIt->first->getOutVertices()[ZIt->second];
    auto NodeIt = Nodes->begin();
    DyckGraphNode *X = *(NodeIt);
    NodeIt++;
    // outs()<<"HERE0.2\n"; outs().flush();
    DyckGraphNode *Y = *(NodeIt);
    if (X->degree() < Y->degree()) {
      DyckGraphNode *Temp = X;
      X = Y;
      Y = Temp;
    }
    // outs()<<"HERE0.3\n"; outs().flush();
    assert(X != Y);
    Vertices.erase(Y);
    auto *Vals = Y->getEquivalentSet();
    for (auto &Val : *Vals) {
      ValVertexMap[Val] = X;
    }
    // outs()<<"HERE0.4\n"; outs().flush();
    Y->mvEquivalentSetTo(X /*->getRepresentative()*/);
    // outs()<<"HERE1\n"; outs().flush();
    std::set<void *> &YOutLabels = Y->getOutLabels();
    auto YOIt = YOutLabels.begin();
    while (YOIt != YOutLabels.end()) {
      if (Y->containsTarget(Y, *YOIt)) {
        if (!X->containsTarget(X, *YOIt)) {
          X->addTarget(X, *YOIt);
          if (X->outNumVertices(*YOIt) > 1 &&
              !containsInWorkList(Worklist, X, *YOIt)) {
            Worklist.insert(std::pair<DyckGraphNode *, void *>(X, *YOIt));
          }
        }
        Y->removeTarget(Y, *YOIt);
        if (Y->outNumVertices(*YOIt) < 2) {
          removeFromWorkList(Worklist, Y, *YOIt);
        }
      }
      YOIt++;
    }
    // outs()<<"HERE2\n"; outs().flush();
    YOIt = YOutLabels.begin();
    while (YOIt != YOutLabels.end()) {
      std::set<DyckGraphNode *> *Ws = &Y->getOutVertices()[*YOIt];
      auto W = Ws->begin();
      while (W != Ws->end()) {
        if (!X->containsTarget(*W, *YOIt)) {
          X->addTarget(*W, *YOIt);
          if (X->outNumVertices(*YOIt) > 1 &&
              !containsInWorkList(Worklist, X, *YOIt)) {
            Worklist.insert(std::pair<DyckGraphNode *, void *>(X, *YOIt));
          }
        }
        // cannot use removeTarget function, which will affect iterator
        // y remove target *w
        DyckGraphNode *WTemp = *W;
        Ws->erase(W++);
        // *w remove src y
        ((WTemp)->getInVertices())[*YOIt].erase(Y);
        if (Y->outNumVertices(*YOIt) < 2) {
          removeFromWorkList(Worklist, Y, *YOIt);
        }
      }
      YOIt++;
    }
    // outs()<<"HERE3\n"; outs().flush();
    std::set<void *> &YInLabels = Y->getInLabels();
    auto YIIt = YInLabels.begin();
    while (YIIt != YInLabels.end()) {
      std::set<DyckGraphNode *> *Ws = &Y->getInVertices()[*YIIt];
      auto W = Ws->begin();
      while (W != Ws->end()) {
        if (!(*W)->containsTarget(X, *YIIt)) {
          (*W)->addTarget(X, *YIIt);
        }

        // cannot use removeTarget function, which will affect iterator
        DyckGraphNode *WTemp = *W;
        Ws->erase(W++);
        ((WTemp)->getOutVertices())[*YIIt].erase(Y);
        if ((WTemp)->outNumVertices(*YIIt) < 2) {
          removeFromWorkList(Worklist, WTemp, *YIIt);
        }
      }

      YIIt++;
    }
    delete Y;
  }
  return Ret;
}

std::pair<DyckGraphNode *, bool>
DyckGraph::retrieveDyckVertex(void *Val, const char *Name) {
  if (Val == nullptr) {
    auto *Node = new DyckGraphNode(nullptr);
    Vertices.insert(Node);
    return std::make_pair(Node, false);
  }

  auto It = ValVertexMap.find(Val);
  if (It != ValVertexMap.end()) {
    return std::make_pair(It->second, true);
  } else {
    auto *Node = new DyckGraphNode(Val, Name);
    Vertices.insert(Node);
    ValVertexMap.insert(std::pair<void *, DyckGraphNode *>(Val, Node));
    return std::make_pair(Node, false);
  }
}

DyckGraphNode *DyckGraph::findDyckVertex(void *Val) {
  auto It = ValVertexMap.find(Val);
  if (It != ValVertexMap.end()) {
    return It->second;
  }
  return nullptr;
}

unsigned int DyckGraph::numVertices() { return Vertices.size(); }

unsigned int DyckGraph::numEquivalentClasses() { return Vertices.size(); }

std::set<DyckGraphNode *> &DyckGraph::getVertices() { return Vertices; }

void DyckGraph::validation(const char *File, int Line) {
  printf("Start validation... ");
  std::set<DyckGraphNode *> &Reps = this->getVertices();
  auto RepsIt = Reps.begin();
  while (RepsIt != Reps.end()) {
    DyckGraphNode *Rep = *RepsIt;
    auto *RepVal = Rep->getEquivalentSet();
    for (auto *Val : *RepVal)
      assert(ValVertexMap[Val] == Rep);
    RepsIt++;
  }
  printf("Done!\n\n");
}

void DyckGraph::getReachableVertices(const std::set<DyckGraphNode *> &Sources,
                                     std::set<DyckGraphNode *> &Reachable) {
  std::stack<DyckGraphNode *> WorkStack;
  for (auto *N : Sources)
    if (N)
      WorkStack.push(N);
  std::set<DyckGraphNode *> Visited;
  while (!WorkStack.empty()) {
    DyckGraphNode *Top = WorkStack.top();
    WorkStack.pop();
    if (Visited.count(Top))
      continue;
    Visited.insert(Top);

    std::set<DyckGraphNode *> Tars;
    Top->getOutVertices(&Tars);
    auto TIt = Tars.begin();
    while (TIt != Tars.end()) {
      DyckGraphNode *DGN = (*TIt);
      if (Visited.find(DGN) == Visited.end())
        WorkStack.push(DGN);
      TIt++;
    }
  }
}

void DyckGraph::getReachableVertices(DyckGraphNode *Source,
                                     std::set<DyckGraphNode *> &Reachable) {
  if (!Source)
    return;
  std::set<DyckGraphNode *> Srcs;
  Srcs.insert(Source);
  getReachableVertices(Srcs, Reachable);
}