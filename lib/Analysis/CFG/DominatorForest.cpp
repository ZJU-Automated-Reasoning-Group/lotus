/*
 * Copyright 2016 - 2024  Angelo Matni, Simone Campanoni
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do
 so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "Analysis/CFG/DominatorForest.h"

namespace noelle {

DominatorForest::DominatorForest(llvm::DominatorTree &DT)
    : DominatorForest{collectNodesOfTree<llvm::DominatorTree>(DT)} {
  this->post = false;
  return;
}

DominatorForest::DominatorForest(llvm::PostDominatorTree &PDT)
    : DominatorForest{collectNodesOfTree<PostDominatorTree>(PDT)} {
  this->post = true;
  return;
}

DominatorForest::DominatorForest(std::set<DTAliases::Node *> nodeSubset)
    : nodes{}, bbNodeMap{}, post{false} {
  this->cloneLLVMNodes(nodeSubset);
  return;
}

DominatorForest::DominatorForest(DominatorForest &DTS,
                                 std::set<BasicBlock *> &bbSubset)
    : DominatorForest{filterNodes(DTS.nodes, bbSubset)} {
  // Fix #6: propagate the post flag from the source forest so that
  // dominates(Instruction*, Instruction*) uses the correct direction
  // for subset forests built from a PostDominatorTree.
  this->post = DTS.post;
  return;
}

DominatorForest::DominatorForest(std::set<DominatorNode *> nodeSubset)
    : nodes{}, bbNodeMap{}, post{false} {
  this->cloneNodes<DominatorNode>(nodeSubset);
  return;
}

DominatorForest::~DominatorForest() { destroyNodes(); }

// Bug 10 fix: destroyNodes() centralises the cleanup logic used by both the
// destructor and the copy-assignment operator.
void DominatorForest::destroyNodes() {
  for (auto *node : nodes)
    delete node;
  nodes.clear();
  bbNodeMap.clear();
}

// Bug 10 fix: deep-copy helper.  Clones every DominatorNode, rebuilds the
// bbNodeMap, and re-wires parent/child pointers so the copy is fully
// independent of the source.
void DominatorForest::copyFrom(const DominatorForest &other) {
  post = other.post;

  // First pass: clone every node (parent/children not yet wired).
  std::unordered_map<const DominatorNode *, DominatorNode *> nodeMap;
  for (auto *src : other.nodes) {
    auto *copy =
        new DominatorNode(*src); // copies B and level; parent/children reset
    nodeMap[src] = copy;
    nodes.insert(copy);
    if (copy->B)
      bbNodeMap[copy->B] = copy;
  }

  // Second pass: wire parent and children using the clone map.
  for (auto *src : other.nodes) {
    auto *copy = nodeMap[src];
    if (src->parent) {
      auto it = nodeMap.find(src->parent);
      copy->parent = (it != nodeMap.end()) ? it->second : nullptr;
    }
    copy->children.clear();
    for (auto *child : src->children) {
      auto it = nodeMap.find(child);
      if (it != nodeMap.end())
        copy->children.push_back(it->second);
    }
  }
}

// Bug 10 fix: copy constructor — performs a deep copy via copyFrom().
DominatorForest::DominatorForest(const DominatorForest &other)
    : nodes{}, bbNodeMap{}, post{false} {
  copyFrom(other);
}

// Bug 10 fix: copy-assignment operator — destroy existing nodes, then deep
// copy.
DominatorForest &DominatorForest::operator=(const DominatorForest &other) {
  if (this != &other) {
    destroyNodes();
    copyFrom(other);
  }
  return *this;
}

// Move constructor: steal the source's data, leave it in a valid empty state.
DominatorForest::DominatorForest(DominatorForest &&other) noexcept
    : nodes{std::move(other.nodes)}, bbNodeMap{std::move(other.bbNodeMap)},
      post{other.post} {
  other.post = false;
}

// Move-assignment operator.
DominatorForest &DominatorForest::operator=(DominatorForest &&other) noexcept {
  if (this != &other) {
    destroyNodes();
    nodes = std::move(other.nodes);
    bbNodeMap = std::move(other.bbNodeMap);
    post = other.post;
    other.post = false;
  }
  return *this;
}

void DominatorForest::transferToClones(
    std::unordered_map<BasicBlock *, BasicBlock *> &bbCloneMap) {
  for (auto *node : nodes) {
    assert(bbCloneMap.find(node->B) != bbCloneMap.end());
    node->B = bbCloneMap[node->B];
  }
}

template <typename TreeType>
std::set<DTAliases::Node *> DominatorForest::collectNodesOfTree(TreeType &T) {
  std::set<DTAliases::Node *> nodes;
  std::vector<DTAliases::Node *> worklist;
  for (BasicBlock *b : T.roots())
    worklist.push_back(T.getNode(b));

  /*
   * Workaround: An empty "exit node" exists for PostDominatorForest that isn't
   * accessible via getRoots()
   */
  worklist.push_back(T.getRootNode());

  while (worklist.size() != 0) {
    auto *node = worklist.back();
    worklist.pop_back();
    nodes.insert(node);
    for (auto *child : *node)
      worklist.push_back(child);
  }

  return nodes;
}

std::set<DominatorNode *>
DominatorForest::filterNodes(std::set<DominatorNode *> &nodes,
                             std::set<BasicBlock *> &bbSubset) {
  std::set<DominatorNode *> nodesSubset;
  for (auto *node : nodes) {
    if (bbSubset.find(node->B) != bbSubset.end()) {
      nodesSubset.insert(node);
    }
  }
  return nodesSubset;
}

void DominatorForest::cloneLLVMNodes(
    std::set<DTAliases::Node *> &nodesToClone) {

  /*
   * Clone nodes using DomNodeSummary constructors. Track cloned pairs in map
   */
  std::unordered_map<DTAliases::Node *, DominatorNode *> nodeMap;
  for (auto *node : nodesToClone) {
    auto *summary = new DominatorNode(*node);
    nodeMap[node] = summary;
    this->nodes.insert(summary);
    this->bbNodeMap[summary->B] = summary;
  }

  /*
   * Populate parent, child relations between cloned nodes.
   * Note the optional nature of these connections. It is possible
   * that only a subset of the tree is being cloned
   */
  for (auto *node : nodesToClone) {
    auto *summary = nodeMap[node];
    for (auto *child : node->children()) {
      if (nodeMap.find(child) == nodeMap.end())
        continue;
      auto *childSummary = nodeMap[child];
      childSummary->parent = summary;
      summary->children.push_back(childSummary);
    }
  }

  return;
}

template <typename NodeType>
void DominatorForest::cloneNodes(std::set<NodeType *> &nodesToClone) {

  /*
   * Clone nodes using DominatorNode constructors. Track cloned pairs in map
   */
  std::unordered_map<NodeType *, DominatorNode *> nodeMap;
  for (auto *node : nodesToClone) {
    auto *summary = new DominatorNode(*node);
    nodeMap[node] = summary;
    this->nodes.insert(summary);
    this->bbNodeMap[summary->B] = summary;
  }

  /*
   * Populate parent, child relations between cloned nodes.
   * Note the optional nature of these connections. It is possible
   * that only a subset of the tree is being cloned
   */
  for (auto *node : nodesToClone) {
    auto *summary = nodeMap[node];
    auto children = node->getChildren();
    for (auto *child : children) {
      if (nodeMap.find(child) == nodeMap.end())
        continue;
      auto *childSummary = nodeMap[child];
      childSummary->parent = summary;
      summary->children.push_back(childSummary);
    }
  }
}

DominatorNode *DominatorForest::getNode(BasicBlock *B) const {
  auto nodeIter = bbNodeMap.find(B);
  return nodeIter == bbNodeMap.end() ? nullptr : nodeIter->second;
}

bool DominatorForest::dominates(Instruction *I, Instruction *J) const {
  auto *B1 = I->getParent();
  auto *B2 = J->getParent();

  /*
   * Check if the instructions belong to the same basic block.
   */
  if (B1 == B2) {

    // Fix #5: dominates(I, I) must return true (reflexivity).
    // The old code advanced firstOne before checking, so it never matched
    // when I == J, returning false — violating the standard dominance
    // convention.  Start the scan at I itself (not I->getNextNode()).
    for (auto *cur = I; cur != nullptr; cur = cur->getNextNode()) {
      if (cur == J) {
        // J is found at or after I in the block.
        // For a dominator tree:      I dominates J → true.
        // For a post-dominator tree: I does NOT post-dominate J → false.
        return !this->post;
      }
    }

    // J was not found at or after I, so J precedes I in the block.
    // For a dominator tree:      I does NOT dominate J → false.
    // For a post-dominator tree: I post-dominates J → true.
    return this->post;
  }

  /*
   * The instructions belong to different basic blocks.
   *
   * Check if B1 dominates B2.
   */
  auto d = this->dominates(B1, B2);

  return d;
}

bool DominatorForest::dominates(BasicBlock *B1, BasicBlock *B2) const {
  auto *nodeB1 = this->getNode(B1);
  auto *nodeB2 = this->getNode(B2);
  // Bug 6 fix: when either block is absent from the forest (e.g., because this
  // is a subset forest and the block was pruned out), return false instead of
  // asserting. A missing block cannot dominate or be dominated within this
  // forest, so false is the correct conservative answer.
  if (!nodeB1 || !nodeB2)
    return false;
  return this->dominates(nodeB1, nodeB2);
}

bool DominatorForest::strictlyDominates(Instruction *I, Instruction *J) const {
  if (I == J) {
    return false;
  }

  return this->dominates(I, J);
}

bool DominatorForest::strictlyDominates(BasicBlock *B1, BasicBlock *B2) const {
  if (B1 == B2) {
    return false;
  }

  return this->dominates(B1, B2);
}

bool DominatorForest::dominates(DominatorNode *node1,
                                DominatorNode *node2) const {
  std::queue<DominatorNode *> worklist;
  worklist.push(node1);
  while (!worklist.empty()) {
    auto *node = worklist.front();
    worklist.pop();

    if (node == node2) {
      return true;
    }
    for (auto *child : node->children)
      worklist.push(child);
  }

  return false;
}

std::set<DominatorNode *>
DominatorForest::dominates(DominatorNode *node) const {
  std::set<DominatorNode *> dominators;
  while (node) {
    dominators.insert(node);
    node = node->parent;
  }
  return dominators;
}

std::set<Instruction *>
DominatorForest::getDominatorsOf(const std::set<Instruction *> &s,
                                 BasicBlock *dominatedBB) const {
  std::set<Instruction *> r{};

  /*
   * Consider all elements of the set.
   */
  for (auto *value : s) {

    /*
     * Check if @value dominates @dominatedBB
     */
    auto *valueBB = value->getParent();
    if (this->dominates(valueBB, dominatedBB)) {
      r.insert(value);
    }
  }

  return r;
}

std::set<BasicBlock *> DominatorForest::getDescendants(BasicBlock *bb) const {
  std::set<BasicBlock *> ds;

  /*
   * Fetch the node that represents @bb.
   */
  auto *bbNode = this->getNode(bb);
  assert(bbNode != nullptr);

  this->addDescendants(bbNode, ds);

  return ds;
}

void DominatorForest::addDescendants(DominatorNode *n,
                                     std::set<BasicBlock *> &ds) const {

  /*
   * Add itself.
   */
  ds.insert(n->getBlock());

  /*
   * Iterate over children.
   */
  for (auto *child : n->getChildren()) {
    this->addDescendants(child, ds);
  }

  return;
}

std::set<Instruction *>
DominatorForest::getInstructionsThatDoNotDominateAnyOther(
    const std::set<Instruction *> &s) const {
  std::set<Instruction *> r{};

  /*
   * Consider all elements of the set.
   */
  for (auto *value : s) {

    /*
     * Check if @value dominates any other
     */
    auto isDominatingOthers = false;
    for (auto *otherValue : s) {
      if (value == otherValue) {
        continue;
      }
      if (!this->dominates(value, otherValue)) {
        continue;
      }
      isDominatingOthers = true;
      break;
    }
    if (isDominatingOthers) {
      continue;
    }

    /*
     * Value does not dominate anyone
     */
    r.insert(value);
  }

  return r;
}

BasicBlock *DominatorForest::findNearestCommonDominator(BasicBlock *B1,
                                                        BasicBlock *B2) const {
  assert(B1 != nullptr);
  assert(B2 != nullptr);

  // Bug 7 fix: getNode() returns nullptr for blocks absent from the forest
  // (e.g., subset forests where the true common dominator was pruned out).
  // Return nullptr instead of asserting / dereferencing a null pointer so
  // callers can handle the "no common dominator in this forest" case.
  auto *n1 = this->getNode(B1);
  auto *n2 = this->getNode(B2);
  if (!n1 || !n2)
    return nullptr;

  auto *c = findNearestCommonDominator(n1, n2);
  // c may be nullptr when the two nodes share no common ancestor within the
  // (possibly pruned) forest — return nullptr and let the caller decide.
  return c ? c->B : nullptr;
}

DominatorNode *
DominatorForest::findNearestCommonDominator(DominatorNode *node1,
                                            DominatorNode *node2) const {

  /*
   * Helpers to determine whether a node n dominates node2
   */
  auto dominatorsOf2 = this->dominates(node2);
  auto dominates2 = [&](DominatorNode *node) -> bool {
    return dominatorsOf2.find(node) != dominatorsOf2.end();
  };

  /*
   * Traversal of parents of node1 to find common dominator
   */
  DominatorNode *node = node1;
  while (node && !dominates2(node))
    node = node->parent;
  return node;
}

raw_ostream &DominatorForest::print(raw_ostream &stream,
                                    std::string prefixToUse) const {
  for (auto *node : nodes) {
    node->print(stream, prefixToUse);
  }
  return stream;
}

} // namespace noelle