/// @file ICFGBuilder.cpp
/// @brief Implementation of ICFG builder for constructing interprocedural CFG.

#include "IR/ICFG/ICFGBuilder.h"

#include "IR/ICFG/GraphAnalysis.h"

#include <queue>
#include <set>

#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace {

bool isExceptionalFunctionExitInst(const Instruction &inst) {
  if (isa<ResumeInst>(inst))
    return true;

  if (const auto *cleanupRet = dyn_cast<CleanupReturnInst>(&inst))
    return cleanupRet->unwindsToCaller();

  return false;
}

bool blockHasReturningCall(const BasicBlock &bb) {
  for (const Instruction &inst : bb) {
    const auto *call = dyn_cast<CallBase>(&inst);
    if (call && !call->doesNotReturn())
      return true;
  }
  return false;
}

SmallVector<const Function *, 8> collectRootFunctions(Module *module) {
  SmallVector<const Function *, 8> roots;
  if (!module)
    return roots;

  if (const Function *mainFunc = module->getFunction("main")) {
    if (!mainFunc->isDeclaration()) {
      roots.push_back(mainFunc);
      return roots;
    }
  }

  SmallPtrSet<const Function *, 16> definedFuncs;
  SmallPtrSet<const Function *, 16> calledFuncs;
  for (const Function &F : *module) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;
    definedFuncs.insert(&F);
  }

  for (const Function &F : *module) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;
    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        const auto *call = dyn_cast<CallBase>(&I);
        if (!call)
          continue;
        const Function *callee = call->getCalledFunction();
        if (!callee || callee->isDeclaration() || callee->isIntrinsic())
          continue;
        calledFuncs.insert(callee);
      }
    }
  }

  for (const Function *F : definedFuncs) {
    if (!calledFuncs.count(F))
      roots.push_back(F);
  }
  if (!roots.empty())
    return roots;

  for (const Function *F : definedFuncs)
    roots.push_back(F);
  return roots;
}

void connectNormalContinuation(ICFG *icfg, const CallBase *call,
                               ICFGNode *returnSiteNode) {
  if (!icfg || !call || !returnSiteNode)
    return;

  if (const auto *invokeInst = dyn_cast<InvokeInst>(call)) {
    ICFGNode *continuationNode =
        icfg->getIntraBlockNode(invokeInst->getNormalDest());
    if (continuationNode)
      icfg->addIntraEdge(returnSiteNode, continuationNode);
    return;
  }

  const BasicBlock *callBB = call->getParent();
  for (auto succIt = succ_begin(callBB), succEnd = succ_end(callBB);
       succIt != succEnd; ++succIt) {
    ICFGNode *continuationNode = icfg->getIntraBlockNode(*succIt);
    if (continuationNode)
      icfg->addIntraEdge(returnSiteNode, continuationNode);
  }
}

void connectUnwindContinuation(ICFG *icfg, const InvokeInst *invokeInst,
                               ICFGNode *unwindSiteNode) {
  if (!icfg || !invokeInst || !unwindSiteNode)
    return;

  ICFGNode *continuationNode =
      icfg->getIntraBlockNode(invokeInst->getUnwindDest());
  if (continuationNode)
    icfg->addIntraEdge(unwindSiteNode, continuationNode);
}

void connectFunctionReturnExits(ICFG *icfg, const Function *callee) {
  if (!icfg || !callee || callee->isDeclaration())
    return;

  ICFGNode *exitNode = icfg->getFunExitICFGNode(callee);
  if (!exitNode)
    return;

  for (const BasicBlock &bb : *callee) {
    if (!isa<ReturnInst>(bb.getTerminator()))
      continue;

    ICFGNode *retBlockNode = icfg->getIntraBlockNode(&bb);
    if (retBlockNode)
      icfg->addIntraEdge(retBlockNode, exitNode);
  }
}

void connectFunctionExceptionalExits(ICFG *icfg, const Function *callee) {
  if (!icfg || !callee || callee->isDeclaration())
    return;

  ICFGNode *unwindExitNode = icfg->getFunUnwindExitICFGNode(callee);
  if (!unwindExitNode)
    return;

  for (const BasicBlock &bb : *callee) {
    const Instruction *terminator = bb.getTerminator();
    if (!terminator || !isExceptionalFunctionExitInst(*terminator))
      continue;

    ICFGNode *exitBlockNode = icfg->getIntraBlockNode(&bb);
    if (exitBlockNode)
      icfg->addIntraEdge(exitBlockNode, unwindExitNode);
  }
}

} // namespace

/// @brief Builds the ICFG for all non-declaration functions in the module.
void ICFGBuilder::build(llvm::Module *module) {
  ICFGNode *globalInitNode = icfg->getGlobalInitICFGNode();
  for (const Function *root : collectRootFunctions(module)) {
    ICFGNode *entryNode = icfg->getFunEntryICFGNode(root);
    if (globalInitNode && entryNode)
      icfg->addIntraEdge(globalInitNode, entryNode);
  }

  for (auto &func : *module) {
    if (func.isDeclaration() || func.isIntrinsic())
      continue;

    processFunction(&func);
  }

  if (_removeCycleAfterBuild) {

    removeIntraBlockCycle();
    removeInterCallCycle();

    setRemoveCycleAfterBuild(false);
  }
}

void ICFGBuilder::processFunction(const llvm::Function *func) {
  ICFGNode *funEntryNode = icfg->getFunEntryICFGNode(func);
  ICFGNode *entryBlockNode = getOrAddIntraBlockICFGNode(&func->getEntryBlock());
  if (funEntryNode && entryBlockNode)
    icfg->addIntraEdge(funEntryNode, entryBlockNode);
  (void)icfg->getFunExitICFGNode(func);

  std::queue<const llvm::BasicBlock *> worklist;
  worklist.push(&func->getEntryBlock());

  std::set<const llvm::BasicBlock *> visited;
  while (!worklist.empty()) {
    const auto *bb = worklist.front();
    worklist.pop();

    if (visited.find(bb) != visited.end())
      continue;

    visited.insert(bb);
    ICFGNode *srcNode = getOrAddIntraBlockICFGNode(bb);
    bool suppressRawSuccEdges = blockHasReturningCall(*bb);

    for (auto succIt = succ_begin(bb), e = succ_end(bb); succIt != e; ++succIt) {
      const auto *succBB = *succIt;
      ICFGNode *dstNode = getOrAddIntraBlockICFGNode(succBB);
      if (!suppressRawSuccEdges)
        icfg->addIntraEdge(srcNode, dstNode);
      worklist.push(succBB);
    }

    for (const Instruction &inst : *bb) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call)
        continue;

      ICFGNode *returnSiteNode = nullptr;
      ICFGNode *unwindSiteNode = nullptr;
      if (!call->doesNotReturn()) {
        returnSiteNode = icfg->getRetICFGNode(call);
        connectNormalContinuation(icfg, call, returnSiteNode);

        if (const auto *invokeInst = dyn_cast<InvokeInst>(call)) {
          unwindSiteNode = icfg->getUnwindICFGNode(call);
          connectUnwindContinuation(icfg, invokeInst, unwindSiteNode);
        }
      }

      Function *calledFunc = call->getCalledFunction();
      if (!calledFunc || calledFunc->isDeclaration()) {
        if (returnSiteNode)
          icfg->addIntraEdge(srcNode, returnSiteNode);
        if (unwindSiteNode)
          icfg->addIntraEdge(srcNode, unwindSiteNode);
        continue;
      }

      ICFGNode *calleeEntryNode = icfg->getFunEntryICFGNode(calledFunc);
      if (calleeEntryNode)
        icfg->addCallEdge(srcNode, calleeEntryNode, call);

      if (returnSiteNode) {
        connectFunctionReturnExits(icfg, calledFunc);
        icfg->addRetEdge(icfg->getFunExitICFGNode(calledFunc), returnSiteNode,
                         call);
      }

      if (unwindSiteNode) {
        connectFunctionExceptionalExits(icfg, calledFunc);
        icfg->addExcRetEdge(icfg->getFunUnwindExitICFGNode(calledFunc),
                            unwindSiteNode, call);
      }
    }
  }
}

void ICFGBuilder::removeIntraBlockCycle() {

  const auto &funcMap = icfg->getFunctionEntryMap();

  for (const auto &p : funcMap) {
    const Function *func = p.first;

    std::set<ICFGEdge *> res;
    findFunctionBackedgesIntraICFG(icfg, func, res);

    for (auto *edge : res) {

      icfg->removeICFGEdge(edge);
    }
  }
}

void ICFGBuilder::removeInterCallCycle() {

  const auto &funcMap = icfg->getFunctionEntryMap();

  for (const auto &p : funcMap) {
    const Function *func = p.first;

    std::set<ICFGEdge *> res;
    findFunctionBackedgesInterICFG(icfg, func, res);

    for (auto *edge : res) {

      icfg->removeICFGEdge(edge);
    }
  }
}

void ICFGBuilder::setRemoveCycleAfterBuild(bool b) {
  _removeCycleAfterBuild = b;
}
