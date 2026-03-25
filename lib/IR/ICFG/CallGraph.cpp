/// @file CallGraph.cpp
/// @brief Implementation of custom call graph for Lotus framework.

#include "IR/ICFG/CallGraph.h"

#include <algorithm>
#include <iostream>

using namespace llvm;

//===----------------------------------------------------------------------===//
// Implementations of the LTCallGraph class methods.
//

LTCallGraph::LTCallGraph(Module &M)
    : M(M), ExternalCallingNode(getOrInsertFunction(nullptr)),
      CallsExternalNode(std::make_unique<LTCallGraphNode>(nullptr)) {
  // Add every function to the call graph.
  for (Function &F : M)
    addToCallGraph(&F);
}

LTCallGraph::LTCallGraph(LTCallGraph &&Arg)
    : M(Arg.M), FunctionMap(std::move(Arg.FunctionMap)),
      ExternalCallingNode(Arg.ExternalCallingNode),
      CallsExternalNode(std::move(Arg.CallsExternalNode)) {
  Arg.FunctionMap.clear();
  Arg.ExternalCallingNode = nullptr;
}

LTCallGraph::~LTCallGraph() {
  // CallsExternalNode is not in the function map, delete it explicitly.
  if (CallsExternalNode)
    CallsExternalNode->allReferencesDropped();

  // Reset all node's use counts to zero before deleting them to prevent an
  // assertion from firing.
  for (auto &I : FunctionMap)
    I.second->allReferencesDropped();
}

void LTCallGraph::dump() const {

  SmallVector<LTCallGraphNode *, 16> Nodes;
  Nodes.reserve(FunctionMap.size());

  for (const auto &I : *this)
    Nodes.push_back(I.second.get());

  std::sort(Nodes.begin(), Nodes.end(),
            [](LTCallGraphNode *LHS, LTCallGraphNode *RHS) {
              if (Function *LF = LHS->getFunction())
                if (Function *RF = RHS->getFunction())
                  return LF->getName() < RF->getName();

              return RHS->getFunction() != nullptr;
            });

  for (auto *CN : Nodes)
    CN->dump();
}

void LTCallGraph::addToCallGraph(Function *F) {

  LTCallGraphNode *Node = getOrInsertFunction(F);

  // If this function has external linkage or has its address taken, anything
  // could call it.
  // we omit address taken here because we want explicit call graph
  //    if (!F->hasLocalLinkage() || F->hasAddressTaken())
  //        ExternalCallingNode->addCalledFunction(CallSite(), Node);

  // If this function is not defined in this translation unit, it could call
  // anything.
  if (F->isDeclaration() && !F->isIntrinsic())
    Node->addCalledFunction(nullptr, CallsExternalNode.get());

  // Look for calls by this function.
  for (BasicBlock &BB : *F)
    for (Instruction &I : BB) {
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        const Function *Callee = CB->getCalledFunction();
        if (!Callee)
          // Indirect calls of intrinsics are not allowed so no need to check.
          // We can be more precise here by using TargetArg returned by
          // Intrinsic::isLeaf.
          Node->addCalledFunction(CB, CallsExternalNode.get());
        else if (!Callee->isIntrinsic())
          Node->addCalledFunction(CB, getOrInsertFunction(Callee));
      }
    }
}

/// @brief Removes a function from the module and destroys its call graph node.
Function *LTCallGraph::removeFunctionFromModule(LTCallGraphNode *CGN) {
  assert(CGN->empty() && "Cannot remove function from call "
                         "graph if it references other functions!");
  Function *F = CGN->getFunction(); // Get the function for the call graph node
  FunctionMap.erase(F);             // Remove the call graph node from the map

  M.getFunctionList().remove(F);
  return F;
}

/// spliceFunction - Replace the function represented by this node by another.
/// This does not rescan the body of the function, so it is suitable when
/// splicing the body of the old function to the new while also updating all
/// callers from old to new.
void LTCallGraph::spliceFunction(const Function *From, const Function *To) {
  assert(FunctionMap.count(From) && "No LTCallGraphNode for function!");
  assert(!FunctionMap.count(To) &&
         "Pointing LTCallGraphNode at a function that already exists");
  FunctionMapTy::iterator I = FunctionMap.find(From);
  I->second->F = const_cast<Function *>(To);
  FunctionMap[To] = std::move(I->second);
  FunctionMap.erase(I);
}

// getOrInsertFunction - This method is identical to calling operator[], but
// it will insert a new LTCallGraphNode for the specified function if one does
// not already exist.
LTCallGraphNode *LTCallGraph::getOrInsertFunction(const Function *F) {
  auto &CGN = FunctionMap[F];
  if (CGN)
    return CGN.get();

  assert((!F || F->getParent() == &M) && "Function not in current module!");
  CGN = std::make_unique<LTCallGraphNode>(const_cast<Function *>(F));
  return CGN.get();
}

void LTCallGraph::addResolvedCallEdge(const Instruction *CS,
                                      const Function *Caller,
                                      const Function *Callee) {
  if (!CS || !Caller || !Callee)
    return;

  LTCallGraphNode *callerNode = getOrInsertFunction(Caller);
  LTCallGraphNode *calleeNode = getOrInsertFunction(Callee);
  if (!callerNode || !calleeNode)
    return;
  if (callerNode->hasCallEdge(CS, calleeNode))
    return;

  callerNode->addCalledFunction(CS, calleeNode);
}

/// removeCallEdgeFor - This method removes the edge in the node for the
/// specified call site.  Note that this method takes linear time, so it
/// should be used sparingly.
void LTCallGraphNode::removeCallEdgeFor(const Instruction *CS) {
  for (CalledFunctionsVector::iterator I = CalledFunctions.begin();; ++I) {
    assert(I != CalledFunctions.end() && "Cannot find callsite to remove!");
    if (I->first == CS) {
      I->second->DropRef();
      *I = CalledFunctions.back();
      CalledFunctions.pop_back();
      return;
    }
  }
}

// removeAnyCallEdgeTo - This method removes any call edges from this node to
// the specified callee function.  This takes more time to execute than
// removeCallEdgeTo, so it should not be used unless necessary.
void LTCallGraphNode::removeAnyCallEdgeTo(LTCallGraphNode *Callee) {
  for (unsigned i = 0, e = CalledFunctions.size(); i != e; ++i)
    if (CalledFunctions[i].second == Callee) {
      Callee->DropRef();
      CalledFunctions[i] = CalledFunctions.back();
      CalledFunctions.pop_back();
      --i;
      --e;
    }
}

/// removeOneAbstractEdgeTo - Remove one edge associated with a null callsite
/// from this node to the specified callee function.
void LTCallGraphNode::removeOneAbstractEdgeTo(LTCallGraphNode *Callee) {
  for (CalledFunctionsVector::iterator I = CalledFunctions.begin();; ++I) {
    assert(I != CalledFunctions.end() && "Cannot find callee to remove!");
    CallRecord &CR = *I;
    if (CR.second == Callee && CR.first == nullptr) {
      Callee->DropRef();
      *I = CalledFunctions.back();
      CalledFunctions.pop_back();
      return;
    }
  }
}

/// replaceCallEdge - This method replaces the edge in the node for the
/// specified call site with a new one.  Note that this method takes linear
/// time, so it should be used sparingly.
void LTCallGraphNode::replaceCallEdge(const Instruction *CS,
                                      const Instruction *NewCS,
                                      LTCallGraphNode *NewNode) {
  for (CalledFunctionsVector::iterator I = CalledFunctions.begin();; ++I) {
    assert(I != CalledFunctions.end() && "Cannot find callsite to remove!");
    if (I->first == CS) {
      I->second->DropRef();
      I->first = const_cast<Value *>(static_cast<const Value *>(NewCS));
      I->second = NewNode;
      NewNode->AddRef();
      return;
    }
  }
}

void LTCallGraphNode::dump() const {

  if (auto *f = getFunction())
    std::cout << "Call graph node for function: '" << f->getName().str() << "'";
  else
    std::cout << "Call graph node <<null function>>";

  std::cout << "<<" << this << ">>  #uses=" << getNumReferences() << '\n';

  for (const auto &I : *this) {
    std::cout << "  CS<" << I.first << "> calls ";
    if (Function *FI = I.second->getFunction())
      std::cout << "function '" << FI->getName().str() << "'\n";
    else
      std::cout << "external node\n";
  }
  std::cout << '\n';
}
