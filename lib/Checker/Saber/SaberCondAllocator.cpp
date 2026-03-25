//===- SaberCondAllocator.cpp -- Path condition manipulation-------------===//

#include "Checker/Saber/SaberCondAllocator.h"

#include "Checker/Saber/SaberOptions.h"
#include "IR/SVFG/SVFG.h"

#include <cmath>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace lotus::analysis;
using Condition = SaberCondAllocator::Condition;

static const llvm::Function *getDirectCallee(const llvm::CallBase *cs) {
  if (!cs)
    return nullptr;
  if (const llvm::Function *callee = cs->getCalledFunction())
    return callee;
  const llvm::Value *called = cs->getCalledOperand();
  if (!called)
    return nullptr;
  return llvm::dyn_cast<llvm::Function>(called->stripPointerCasts());
}

static bool isProgExitCall(const llvm::CallBase *cs) {
  if (!cs)
    return false;
  if (const llvm::Function *callee = getDirectCallee(cs)) {
    llvm::StringRef name = callee->getName();
    return name == "exit" || name == "__assert_rtn" ||
           name == "__assert_fail";
  }
  return false;
}

static unsigned getSuccessorIndex(const llvm::BasicBlock *bb,
                                  const llvm::BasicBlock *succ) {
  const llvm::Instruction *term = bb->getTerminator();
  for (unsigned i = 0; i < term->getNumSuccessors(); i++) {
    if (term->getSuccessor(i) == succ)
      return i;
  }
  return 0;
}

SaberCondAllocator::SaberCondAllocator() : totalCondNum_(0), module_(nullptr) {}

void SaberCondAllocator::setModule(llvm::Module *M) { module_ = M; }

void SaberCondAllocator::reset(bool preserveRemovedSUVFEdges) {
  dtCache_.clear();
  pdtCache_.clear();
  loopInfoCache_.clear();
  retBlocksCache_.clear();
  if (!preserveRemovedSUVFEdges)
    removedSUVFEdges.clear();
  bbConds.clear();
  condToInst.clear();
  negCondIds.clear();
  funToExitBBsMap.clear();
  cfConds.clear();
  conditionVec.clear();
  finalCond = getFalseCond();
  totalCondNum_ = 0;
  curEvalInst = nullptr;
  curEvalSVFGNode = nullptr;
  module_ = nullptr;
}

void SaberCondAllocator::setCFCond(const llvm::BasicBlock *bb,
                                   const Condition &cond) {
  if (bb)
    cfConds[bb] = cond;
}

const SaberCondAllocator::Condition &
SaberCondAllocator::getCFCond(const llvm::BasicBlock *bb) const {
  static const Condition kFalse = Condition::getFalseCond();
  if (!bb)
    return kFalse;
  auto it = cfConds.find(bb);
  if (it == cfConds.end())
    return kFalse;
  return it->second;
}

bool SaberCondAllocator::hasCFCond(const llvm::BasicBlock *bb) const {
  return bb && cfConds.find(bb) != cfConds.end();
}

void SaberCondAllocator::clearCFCond() { cfConds.clear(); }

void SaberCondAllocator::setCondInst(Condition cond,
                                     const llvm::Instruction *inst) {
  if (inst)
    condToInst[cond.id()] = inst;
}

void SaberCondAllocator::setNegCondInst(Condition cond,
                                        const llvm::Instruction *inst) {
  negCondIds.insert(cond.id());
  if (inst)
    condToInst[cond.id()] = inst;
}

const llvm::Instruction *SaberCondAllocator::getCondInst(uint32_t id) const {
  auto it = condToInst.find(id);
  return (it != condToInst.end()) ? it->second : nullptr;
}

bool SaberCondAllocator::isNegCond(uint32_t id) const {
  return negCondIds.count(id) != 0;
}

void SaberCondAllocator::initDominatorsForFunction(const llvm::Function *func) {
  if (!func || func->isDeclaration())
    return;
  if (dtCache_.find(func) == dtCache_.end()) {
    auto dt = std::make_unique<llvm::DominatorTree>();
    dt->recalculate(*const_cast<llvm::Function *>(func));
    dtCache_[func] = std::move(dt);
  }
}

void SaberCondAllocator::initPostDominatorsForFunction(
    const llvm::Function *func) {
  if (!func || func->isDeclaration())
    return;
  if (pdtCache_.find(func) == pdtCache_.end()) {
    auto pdt = std::make_unique<llvm::PostDominatorTree>();
    pdt->recalculate(*const_cast<llvm::Function *>(func));
    pdtCache_[func] = std::move(pdt);
  }
}

void SaberCondAllocator::initLoopInfoForFunction(const llvm::Function *func) {
  if (!func || func->isDeclaration())
    return;
  if (loopInfoCache_.find(func) == loopInfoCache_.end()) {
    auto it = dtCache_.find(func);
    if (it != dtCache_.end())
      loopInfoCache_[func] = std::make_unique<llvm::LoopInfo>(*it->second);
  }
}

void SaberCondAllocator::allocate() {
  if (!module_)
    return;

  for (auto &func : *module_) {
    if (func.isDeclaration())
      continue;

    for (const llvm::BasicBlock &bb : func) {
      collectBBCallingProgExit(&bb);
    }

    for (const llvm::BasicBlock &bb : func) {
      allocateForBB(&bb);
    }
  }
}

void SaberCondAllocator::allocateForBB(const llvm::BasicBlock *bb) {
  if (!bb)
    return;

  unsigned succNum = bb->getTerminator()->getNumSuccessors();

  if (succNum > 1) {
    const llvm::Instruction *term = bb->getTerminator();
    double num = log(static_cast<double>(succNum)) / log(2.0);
    unsigned bitNum = static_cast<unsigned>(ceil(num));
    unsigned succIndex = 0;
    std::vector<Condition> selectorConds;
    selectorConds.reserve(bitNum);
    for (unsigned i = 0; i < bitNum; ++i)
      selectorConds.push_back(newCond(term));

    for (const llvm::BasicBlock *succ : llvm::successors(bb)) {
      Condition pathCond = getTrueCond();
      for (unsigned j = 0; j < bitNum; ++j) {
        unsigned tool = 0x01U << j;
        if (tool & succIndex)
          pathCond = condAnd(pathCond, condNeg(selectorConds.at(j)));
        else
          pathCond = condAnd(pathCond, selectorConds.at(j));
      }
      setBranchCond(bb, succ, pathCond);
      ++succIndex;
    }
  }
}

Condition
SaberCondAllocator::getBranchCond(const llvm::BasicBlock *bb,
                                  const llvm::BasicBlock *succ) const {
  if (!bb || !succ)
    return getTrueCond();

  if (bb->getTerminator()->getNumSuccessors() == 1)
    return getTrueCond();

  auto it = bbConds.find(bb);
  if (it == bbConds.end())
    return getTrueCond();

  unsigned pos = getSuccessorIndex(bb, succ);
  auto cit = it->second.find(pos);
  if (cit == it->second.end())
    return getTrueCond();

  return cit->second;
}

Condition SaberCondAllocator::getEvalBrCond(const llvm::BasicBlock *bb,
                                            const llvm::BasicBlock *succ) {
  if (getCurEvalSVFGNode() && getCurEvalSVFGNode()->getValue())
    return evaluateBranchCond(bb, succ);
  else
    return getBranchCond(bb, succ);
}

void SaberCondAllocator::setBranchCond(const llvm::BasicBlock *bb,
                                       const llvm::BasicBlock *succ,
                                       const Condition &cond) {
  if (!bb || !succ)
    return;

  if (bb->getTerminator()->getNumSuccessors() <= 1)
    return;

  unsigned pos = getSuccessorIndex(bb, succ);
  bbConds[bb][pos] = cond;
}

Condition SaberCondAllocator::evaluateBranchCond(const llvm::BasicBlock *bb,
                                                 const llvm::BasicBlock *succ) {
  if (!bb || !succ)
    return getTrueCond();

  if (bb->getTerminator()->getNumSuccessors() == 1)
    return getTrueCond();

  const llvm::Instruction *term = bb->getTerminator();
  if (!term)
    return getBranchCond(bb, succ);

  if (const llvm::BranchInst *br = llvm::dyn_cast<llvm::BranchInst>(term)) {
    if (br->isConditional()) {
      Condition evalLoopExit = evaluateLoopExitBranch(bb, succ);
      if (evalLoopExit != Condition::nullExpr())
        return evalLoopExit;

      Condition evalProgExit = evaluateProgExit(bb, succ);
      if (evalProgExit != Condition::nullExpr())
        return evalProgExit;

      const llvm::Value *cond = br->getCondition();
      if (const llvm::Instruction *condInst =
              llvm::dyn_cast<llvm::Instruction>(cond)) {
        Condition evalTestNull = evaluateTestNullLikeExpr(condInst, succ);
        if (evalTestNull != Condition::nullExpr())
          return evalTestNull;
      }

      return getBranchCond(bb, succ);
    }
  }

  return getBranchCond(bb, succ);
}

bool SaberCondAllocator::isBBCallsProgExit(const llvm::BasicBlock *bb) {
  if (!bb)
    return false;

  const llvm::Function *func = bb->getParent();
  auto it = funToExitBBsMap.find(std::string(func->getName()));
  if (it != funToExitBBsMap.end()) {
    for (const llvm::BasicBlock *exitBB : it->second) {
      if (postDominate(exitBB, bb))
        return true;
    }
  }
  return false;
}

Condition SaberCondAllocator::evaluateProgExit(const llvm::BasicBlock *bb,
                                               const llvm::BasicBlock *succ) {
  if (!bb || !succ)
    return Condition::nullExpr();

  const llvm::Instruction *term = bb->getTerminator();
  if (!term || term->getNumSuccessors() != 2)
    return Condition::nullExpr();

  const llvm::BasicBlock *succ1 = term->getSuccessor(0);
  const llvm::BasicBlock *succ2 = term->getSuccessor(1);
  bool branch1Exit = isBBCallsProgExit(succ1);
  bool branch2Exit = isBBCallsProgExit(succ2);

  if (branch1Exit && !branch2Exit) {
    return (succ == succ1) ? getFalseCond() : getTrueCond();
  } else if (!branch1Exit && branch2Exit) {
    return (succ == succ2) ? getFalseCond() : getTrueCond();
  } else if (branch1Exit && branch2Exit) {
    return getFalseCond();
  }

  return Condition::nullExpr();
}

Condition
SaberCondAllocator::evaluateTestNullLikeExpr(const llvm::Instruction *brInst,
                                             const llvm::BasicBlock *succ) {
  if (!brInst || !succ)
    return Condition::nullExpr();

  const llvm::CmpInst *cmp = llvm::dyn_cast<llvm::CmpInst>(brInst);
  if (!cmp)
    return Condition::nullExpr();

  const llvm::Value *op0 = cmp->getOperand(0);
  const llvm::Value *op1 = cmp->getOperand(1);

  bool op0IsNull = llvm::isa<llvm::ConstantPointerNull>(op0);
  bool op1IsNull = llvm::isa<llvm::ConstantPointerNull>(op1);

  if (!op0IsNull && !op1IsNull)
    return Condition::nullExpr();

  const llvm::Value *nonNullOp = op0IsNull ? op1 : op0;

  if (!getCurEvalSVFGNode() || !getCurEvalSVFGNode()->getValue()) {
    return Condition::nullExpr();
  }
  bool matchCurFlowVal = false;
  if (getCurEvalSVFGNode()->getValue() == nonNullOp)
    matchCurFlowVal = true;
  if (!matchCurFlowVal) {
    for (SVFGEdge *e : getCurEvalSVFGNode()->getOutEdges()) {
      if (e && e->getDstNode() && e->getDstNode()->getValue() == nonNullOp) {
        matchCurFlowVal = true;
        break;
      }
    }
  }
  if (!matchCurFlowVal)
    return Condition::nullExpr();

  const llvm::Instruction *term = brInst->getParent()->getTerminator();
  const llvm::BasicBlock *succ0 = term->getSuccessor(0);

  bool isSucc0NullBranch = (cmp->getPredicate() == llvm::CmpInst::ICMP_EQ);

  if (succ == succ0) {
    return isSucc0NullBranch ? getFalseCond() : getTrueCond();
  } else {
    return isSucc0NullBranch ? getTrueCond() : getFalseCond();
  }
}

Condition
SaberCondAllocator::evaluateLoopExitBranch(const llvm::BasicBlock *bb,
                                           const llvm::BasicBlock *dst) {
  if (!bb || !dst)
    return Condition::nullExpr();

  const llvm::Function *func = bb->getParent();
  if (func != dst->getParent())
    return Condition::nullExpr();

  auto it = loopInfoCache_.find(func);
  if (it == loopInfoCache_.end())
    return Condition::nullExpr();

  llvm::LoopInfo *LI = it->second.get();
  llvm::Loop *loop = LI->getLoopFor(bb);
  if (!loop || loop->getHeader() != bb)
    return Condition::nullExpr();

  llvm::SmallVector<llvm::BasicBlock *, 4> exitbbs;
  loop->getExitBlocks(exitbbs);
  std::set<const llvm::BasicBlock *> filtered;
  for (llvm::BasicBlock *exitBB : exitbbs) {
    if (!isBBCallsProgExit(exitBB))
      filtered.insert(exitBB);
  }
  bool allPostDom = true;
  for (const llvm::BasicBlock *e : filtered) {
    if (!postDominate(dst, e)) {
      allPostDom = false;
      break;
    }
  }
  return allPostDom ? getTrueCond() : Condition::nullExpr();
}

Condition
SaberCondAllocator::ComputeIntraVFGGuard(const llvm::BasicBlock *srcBB,
                                         const llvm::BasicBlock *dstBB) {
  if (!srcBB || !dstBB)
    return getTrueCond();

  const llvm::Function *func = srcBB->getParent();
  if (func != dstBB->getParent())
    return getTrueCond();

  if (postDominate(dstBB, srcBB))
    return getTrueCond();

  CFWorkList worklist;
  worklist.push_back(srcBB);
  setCFCond(srcBB, getTrueCond());

  while (!worklist.empty()) {
    const llvm::BasicBlock *bb = worklist.front();
    worklist.pop_front();
    Condition cond = getCFCond(bb);

    Condition loopExitCond = evaluateLoopExitBranch(bb, dstBB);
    if (loopExitCond != Condition::nullExpr())
      return condAnd(cond, loopExitCond);

    const llvm::Instruction *term = bb->getTerminator();
    for (unsigned i = 0; i < term->getNumSuccessors(); i++) {
      const llvm::BasicBlock *succ = term->getSuccessor(i);
      Condition brCond;
      if (postDominate(succ, bb))
        brCond = getTrueCond();
      else
        brCond = getEvalBrCond(bb, succ);

      Condition succPathCond = condAnd(cond, brCond);
      Condition oldCond = getCFCond(succ);
      Condition newCond = condOr(oldCond, succPathCond);
      if (!isEquivalentBranchCond(oldCond, newCond)) {
        setCFCond(succ, newCond);
        worklist.push_back(succ);
      }
    }
  }

  return getCFCond(dstBB);
}

Condition
SaberCondAllocator::ComputeInterCallVFGGuard(const llvm::BasicBlock *srcBB,
                                             const llvm::BasicBlock *dstBB,
                                             const llvm::BasicBlock *callBB) {
  if (!srcBB || !dstBB || !callBB)
    return getTrueCond();

  const llvm::Function *caller = srcBB->getParent();
  const llvm::Function *callee = dstBB->getParent();
  if (!caller || !callee)
    return getTrueCond();

  const llvm::BasicBlock *funEntryBB = &callee->getEntryBlock();
  if (!funEntryBB)
    return getTrueCond();

  Condition c1 = ComputeIntraVFGGuard(srcBB, callBB);

  Condition entryCFCond = getCFCond(funEntryBB);
  Condition callCFCond = getCFCond(callBB);
  setCFCond(funEntryBB, condOr(entryCFCond, callCFCond));

  Condition c2 = ComputeIntraVFGGuard(funEntryBB, dstBB);

  return condAnd(c1, c2);
}

Condition
SaberCondAllocator::ComputeInterRetVFGGuard(const llvm::BasicBlock *srcBB,
                                            const llvm::BasicBlock *dstBB,
                                            const llvm::BasicBlock *retBB) {
  if (!srcBB || !dstBB || !retBB)
    return getTrueCond();

  const llvm::Function *func = srcBB->getParent();
  if (!func)
    return getTrueCond();

  std::vector<const llvm::BasicBlock *> &retBlocks = retBlocksCache_[func];
  if (retBlocks.empty()) {
    for (const llvm::BasicBlock &bb : *func)
      if (llvm::isa<llvm::ReturnInst>(bb.getTerminator()))
        retBlocks.push_back(&bb);
  }

  if (retBlocks.empty())
    return getTrueCond();

  Condition c1 = getFalseCond();
  Condition exitCFCond = getFalseCond();
  for (const llvm::BasicBlock *rb : retBlocks) {
    clearCFCond();
    Condition toRet = ComputeIntraVFGGuard(srcBB, rb);
    c1 = condOr(c1, toRet);
    exitCFCond = condOr(exitCFCond, getCFCond(rb));
  }

  Condition retCFCond = getCFCond(retBB);
  setCFCond(retBB, condOr(retCFCond, exitCFCond));

  Condition c2 = ComputeIntraVFGGuard(retBB, dstBB);

  return condAnd(c1, c2);
}

Condition
SaberCondAllocator::getPHIComplementCond(const llvm::BasicBlock *BB1,
                                         const llvm::BasicBlock *BB2,
                                         const llvm::BasicBlock *BB0) {
  if (!BB1 || !BB2 || !BB0)
    return getTrueCond();

  if (dominate(BB1, BB2) && !dominate(BB0, BB2)) {
    Condition cond = ComputeIntraVFGGuard(BB1, BB2);
    return condNeg(cond);
  }

  return getTrueCond();
}

bool SaberCondAllocator::isSatisfiable(const Condition &condition) {
  (void)condition;
#ifdef USE_Z3
  if (condition.getExpr().is_true())
    return true;
  if (condition.getExpr().is_false())
    return false;

  z3::solver solver(Z3Expr::context());
  // Set timeout if configured
  if (SaberOptions::z3Timeout() > 0) {
    z3::params params(Z3Expr::context());
    params.set("timeout", SaberOptions::z3Timeout());
    solver.set(params);
  }
  solver.add(condition.getExpr());
  z3::check_result res = solver.check();
  return res == z3::sat || res == z3::unknown;
#else
  return true;
#endif
}

bool SaberCondAllocator::isEquivalentBranchCond(const Condition &lhs,
                                                const Condition &rhs) const {
#ifdef USE_Z3
  if (lhs == rhs)
    return true;

  z3::solver solver(Z3Expr::context());
  // Set timeout if configured
  if (SaberOptions::z3Timeout() > 0) {
    z3::params params(Z3Expr::context());
    params.set("timeout", SaberOptions::z3Timeout());
    solver.set(params);
  }
  solver.add(lhs.getExpr() != rhs.getExpr());
  z3::check_result res = solver.check();
  return res == z3::unsat;
#else
  return lhs == rhs;
#endif
}

void SaberCondAllocator::extractSubConds(const Condition &condition,
                                         NodeBS &support) const {
  (void)condition;
#ifdef USE_Z3
  const auto &expr = condition.getExpr();
  if (expr.num_args() == 1 && isNegCond(expr.id())) {
    support.insert(expr.id());
    return;
  }
  if (expr.num_args() == 0) {
    if (!expr.is_true() && !expr.is_false())
      support.insert(expr.id());
    return;
  }
  for (unsigned i = 0; i < expr.num_args(); i++) {
    Condition arg = expr.arg(i);
    extractSubConds(arg, support);
  }
#else
  support.clear();
#endif
}

void SaberCondAllocator::collectBBCallingProgExit(const llvm::BasicBlock *bb) {
  if (!bb)
    return;

  const llvm::Function *func = bb->getParent();
  if (!func)
    return;

  for (const llvm::Instruction &inst : *bb) {
    if (const llvm::CallBase *cs = llvm::dyn_cast<llvm::CallBase>(&inst)) {
      if (isProgExitCall(cs)) {
        funToExitBBsMap[std::string(func->getName())].insert(bb);
        return;
      }
    }
  }
}

bool SaberCondAllocator::isTestNullExpr(const llvm::Instruction *test) const {
  if (!test)
    return false;
  const llvm::CmpInst *cmp = llvm::dyn_cast<llvm::CmpInst>(test);
  if (!cmp)
    return false;
  return isTestContainsNullAndTheValue(cmp) && isEQCmp(cmp);
}

bool SaberCondAllocator::isTestNotNullExpr(
    const llvm::Instruction *test) const {
  if (!test)
    return false;
  const llvm::CmpInst *cmp = llvm::dyn_cast<llvm::CmpInst>(test);
  if (!cmp)
    return false;
  return isTestContainsNullAndTheValue(cmp) && isNECmp(cmp);
}

bool SaberCondAllocator::isEQCmp(const llvm::CmpInst *cmp) const {
  return cmp && cmp->getPredicate() == llvm::CmpInst::ICMP_EQ;
}

bool SaberCondAllocator::isNECmp(const llvm::CmpInst *cmp) const {
  return cmp && cmp->getPredicate() == llvm::CmpInst::ICMP_NE;
}

bool SaberCondAllocator::isTestContainsNullAndTheValue(
    const llvm::CmpInst *cmp) const {
  if (!cmp || !curEvalSVFGNode)
    return false;

  const llvm::Value *op0 = cmp->getOperand(0);
  const llvm::Value *op1 = cmp->getOperand(1);

  bool op0IsNull = llvm::isa<llvm::ConstantPointerNull>(op0);
  bool op1IsNull = llvm::isa<llvm::ConstantPointerNull>(op1);

  if (!op0IsNull && !op1IsNull)
    return false;

  const llvm::Value *nonNullOp = op0IsNull ? op1 : op0;
  const llvm::Value *curVal = curEvalSVFGNode->getValue();

  if (curVal == nonNullOp)
    return true;

  for (SVFGEdge *e : curEvalSVFGNode->getOutEdges()) {
    if (e && e->getDstNode() && e->getDstNode()->getValue() == nonNullOp)
      return true;
  }

  return false;
}

bool SaberCondAllocator::dominate(const llvm::BasicBlock *bb1,
                                  const llvm::BasicBlock *bb2) const {
  if (!bb1 || !bb2)
    return false;

  const llvm::Function *func = bb1->getParent();
  if (func != bb2->getParent())
    return false;

  auto it = dtCache_.find(func);
  if (it == dtCache_.end())
    return false;

  return it->second->dominates(bb1, bb2);
}

bool SaberCondAllocator::postDominate(const llvm::BasicBlock *bb1,
                                      const llvm::BasicBlock *bb2) const {
  if (!bb1 || !bb2)
    return false;

  const llvm::Function *func = bb1->getParent();
  if (func != bb2->getParent())
    return false;

  auto it = pdtCache_.find(func);
  if (it == pdtCache_.end())
    return false;

  return it->second->dominates(bb1, bb2);
}

Condition SaberCondAllocator::newCond(const llvm::Instruction *inst) {
  (void)inst;
#ifdef USE_Z3
  std::string name = "c" + std::to_string(totalCondNum_++);
  Condition expr = Z3Expr::context().bool_const(name.c_str());
  Condition negCond = Condition::NEG(expr);
  setCondInst(expr, inst);
  setNegCondInst(negCond, inst);
  conditionVec.push_back(expr);
  conditionVec.push_back(negCond);
  return expr;
#else
  totalCondNum_++;
  return getTrueCond();
#endif
}

std::string SaberCondAllocator::dumpCond(const Condition &cond) const {
  return Z3Expr::dumpStr(cond);
}

void SaberCondAllocator::printPathCond() {
  llvm::errs() << "Path conditions:\n";
  for (const auto &bbCond : bbConds) {
    const llvm::BasicBlock *bb = bbCond.first;
    for (const auto &cit : bbCond.second) {
      unsigned i = 0;
      for (const llvm::BasicBlock *succ : llvm::successors(bb)) {
        if (i == cit.first) {
          Condition cond = cit.second;
          llvm::errs() << bb->getName() << "-->" << succ->getName() << ": "
                       << Z3Expr::dumpStr(cond) << "\n";
          break;
        }
        i++;
      }
    }
  }
}
