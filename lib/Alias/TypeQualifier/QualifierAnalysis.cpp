//
// Created by ubuntu on 2/8/18.
//

#include "Alias/TypeQualifier/QualifierAnalysis.h"

#include "Alias/TypeQualifier/Annotation.h"
#include "Alias/TypeQualifier/CallGraph.h"
#include "Alias/TypeQualifier/FunctionSummary.h"
#include "Alias/TypeQualifier/Helper.h"

#include <cstring>
#include <deque>
#include <queue>
#include <set>
#include <stack>

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PatternMatch.h>

#define CAL_STACKVAR
#define RUN_ON_FUNC
using namespace llvm;

std::string testDir = getCurrentWorkingDir() + "/Summary/";

static bool isCastingCompatibleType(Type *T1, Type *T2);

void IterativeModulePass::run(ModuleList &modules) {
  bool changed = false;
  for (auto &entry : modules) {
    doInitialization(entry.first);
  }

  do {
    changed = false;
    for (auto &entry : modules) {
      changed |= doModulePass(entry.first);
    }
  } while (changed);

  for (auto &entry : modules) {
    doFinalization(entry.first);
  }
}

bool QualifierAnalysis::doInitialization(llvm::Module *M) {
#ifdef HEAP_CHECK
  // To filter the heap warning, we need to record every use of the struct.
  const llvm::DataLayout *DL = &(M->getDataLayout());
  for (Module::iterator fi = M->begin(), fe = M->end(); fi != fe; ++fi) {
    Function *F = &*fi;
    if (F->isIntrinsic())
      continue;
    for (inst_iterator i = inst_begin(F), e = inst_end(F); i != e; ++i) {
      if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(&*i)) {
        const Type *type =
            GEPI->getOperand(0)->getType()->getPointerElementType();
        // An array is considered a single variable of its type.
        while (const ArrayType *arrayType = dyn_cast<ArrayType>(type))
          type = arrayType->getPointerElementType();
        if (const StructType *structType = dyn_cast<StructType>(type)) {
          if (!structType->isOpaque() && !structType->isLiteral()) {
            int64_t offset = getGEPOffset(GEPI, DL);
            int fieldNum =
                offsetToFieldNum(GEPI->getOperand(0)->stripPointerCasts(),
                                 offset, DL, &Ctx->structAnalyzer, M);
            for (User *U : GEPI->users()) {
              if (Instruction *Use = dyn_cast<Instruction>(U)) {
                if (isa<LoadInst>(Use) || isa<GetElementPtrInst>(Use) ||
                    isa<StoreInst>(Use)) {
                  Ctx->usedField[structType].insert(fieldNum);
                }
              }
            }
          }
        }
      }
    }
  }
#endif
  return false;
}

void QualifierAnalysis::getGlobals() {
  for (auto item : Ctx->Modules) {
    llvm::Module *M = item.first;
    for (GlobalVariable &G : M->globals()) {
      if (G.hasExternalLinkage())
        Ctx->Gobjs[G.getName().str()] = &G;
    }
#ifdef LOADSUM
    for (Module::iterator f = M->begin(), fe = M->end(); f != fe; ++f) {
      Function *F = &*f;
      getDef(F);
      std::string fname = getScopeName(F, Ctx->funcToMd, Ctx->Funcs);
      std::string sFile = oldSDir + fname + ".sum";
      if (Ctx->incAnalysis) {
        sFile = newSDir + fname + ".sum";
      }
      std::ifstream ifile(sFile);
      OP << "sFile:" << sFile << "\n";
      if (ifile) {
        OP << "file exists!\n";
        // Loading from .sum files disabled (Boost dependency removed).
        ifile.close();
      }
    }
#endif
  }
}

bool QualifierAnalysis::doModulePass(llvm::Module *M) {
  module = M;
  DL = &(M->getDataLayout());
  bool ret = false;

  for (Module::iterator f = M->begin(), fe = M->end(); f != fe; ++f) {
    Function *F = &*f;

#ifdef READYLIST_DEBUG
    OP << "Ready List:\n";
    for (auto rf : Ctx->ReadyList) {
      OP << rf->getName().str() << "\n";
    }
#endif

    if (Ctx->ReadyList.find(F) != Ctx->ReadyList.end() && (!Ctx->Visit[F])) {
      runOnFunction(F, true);
      FCounter++;
      Ctx->Visit[F] = true;
      for (Function *caller : Ctx->CalledMaps[F]) {
        Ctx->RemainedFunction[caller]--;
        if (Ctx->RemainedFunction[caller] == 0) {
          Ctx->ReadyList.insert(caller);
        }
      }
      ret = true;
    }
  }
  return ret;
}

bool QualifierAnalysis::doFinalization(llvm::Module *M) {
  module = M;
  DL = &(M->getDataLayout());
  bool ret = false;
  return ret;
}

void QualifierAnalysis::run() {
  getGlobals();
  int counter = 0;
  Tarjan tarjan(Ctx->CallMaps);
  tarjan.getSCC(Ctx->SCC);

  for (auto *f : Ctx->indFuncs) {
#ifdef RUN_ON_FUNC
    runOnFunction(f, true);
#endif
    Ctx->Visit[f] = true;
    counter++;
    OP << counter << " Functions Finished.\n";
  }
  unsigned temp = 0;
  for (auto sc : Ctx->SCC) {
    llvm::Function *func = sc.back();
    if (sc.size() == 1 && (!Ctx->CallMaps[func].count(func))) {
#ifdef RUN_ON_FUNC
      runOnFunction(func, true);
#endif
      Ctx->Visit[func] = true;
    } else {
      calSumForRec(sc);
    }
    counter += sc.size();
    OP << counter << " Functions Finished.\n";
  }
  return;
}

void QualifierAnalysis::calSumForRec(std::vector<llvm::Function *> &rec) {
  unsigned temp = 1;
  OP << "calculate summary for " << rec.size() << " recursive functions: \n";
  std::queue<llvm::Function *> scQueue;
  std::unordered_map<llvm::Function *, bool> inQueue;
  Ctx->ChangeMap.clear();
  for (auto *item : rec) {
    scQueue.push(item);
    inQueue[item] = true;
    Ctx->ChangeMap[item] = true;
  }
  bool change = false;
  while (!scQueue.empty()) {
    change = false;
    llvm::Function *func = scQueue.front();
    scQueue.pop();
    inQueue[func] = false;

    Summary in;
#ifdef LOADSUM_
    if (Ctx->FSummaries.find(func) == Ctx->FSummaries.end()) {
      /* Name of function summary: relative path + module name + function name +
       * .sum*/
      std::string fname = getScopeName(func, Ctx->funcToMd, Ctx->Funcs);
      std::string fsName = fname + ".sum";

      OP << "fname = " << fname << "\n";
      std::string sFile = oldSDir + fsName + ".sum";
      OP << "sFile = " << sFile << "\n";

      std::ifstream ifile(sFile);
      if (ifile) {
        OP << "file exists!\n";
        // Loading from .sum files disabled (Boost dependency removed).
        ifile.close();
      }
    }
#endif
    if (Ctx->FSummaries.find(func) != Ctx->FSummaries.end()) {
      in.copySummary(in, Ctx->FSummaries[func], func);
    }
#ifdef RUN_ON_FUNC
    runOnFunction(func, false);
#endif
    if (!Ctx->FSummaries[func].equal(in)) {
      scQueue.push(func);
      inQueue[func] = true;
      Ctx->ChangeMap[func] = true;
    } else {
      Ctx->ChangeMap[func] = false;
      for (auto *item : rec) {
        change |= Ctx->ChangeMap[item];
        if (change)
          break;
      }
      if (!change) {
        break;
      }
    }
  } // while (!scQueue.empty())

  printWarnForRec(rec);
  for (auto *item : rec) {
    Ctx->Visit[item] = true;
  }
  // OP << readyCount << " rec functions are done.\n";
}

bool QualifierAnalysis::runOnFunction(llvm::Function *f, bool flag) {
  llvm::Module *M = f->getParent();
  module = M;
  DL = &(M->getDataLayout());

  FuncAnalysis FA(f, Ctx, flag);
  FA.run();
  return false;
}

bool FuncAnalysis::run() {
  buildPtsGraph();
  initSummary();
  computeAASet();
  runBackwardRequirednessAnalysis();
  runForwardQualifierAnalysis();
#ifdef CAL_STACKVAR
  calStackVar();
#endif
  fSummary.copySummary(Ctx->FSummaries[F], fSummary, F);
  std::string FScopeName = getScopeName(F);
  QualifierCheck();
  return false;
}

void FuncAnalysis::buildPtsGraph() {
  PtsGraph initPtsGraph;
  createInitNodes(initPtsGraph);

  for (Function::iterator bb = F->begin(); bb != F->end(); ++bb)
    for (BasicBlock::iterator itr = bb->begin(); itr != bb->end(); itr++) {
      NodeIndex idx = nodeFactory.createValueNode(&*itr);
    }
  std::deque<BasicBlock *> worklist;
  for (Function::iterator bb = F->begin(), be = F->end(); bb != be; ++bb) {
    worklist.push_back(&*bb);
  }

  Instruction *entry = &(F->front().front());
  NodeIndex entryIndex = nodeFactory.getValueNodeFor(entry);
  unsigned count = 0;
  unsigned threshold = 20 * worklist.size();

  while (!worklist.empty() && ++count <= threshold) {
    BasicBlock *BB = worklist.front();
    worklist.pop_front();

    PtsGraph in;
    PtsGraph out;

    // calculates BB in
    if (BB == &F->front()) {
      in = initPtsGraph;
    } else {
      for (auto pi = pred_begin(BB), pe = pred_end(BB); pi != pe; ++pi) {
        const BasicBlock *pred = *pi;
        const Instruction *b = &pred->back();
        unionPtsGraph(in, nPtsGraph[b]);
      }
    }

    bool changed = false;
    for (BasicBlock::iterator ii = BB->begin(), ie = BB->end(); ii != ie;
         ++ii) {
      Instruction *I = &*ii;
      if (I != &BB->front())
        in = nPtsGraph[I->getPrevNode()];

      out = processInstruction(I, in);
      if (I == &BB->back()) {
        PtsGraph old = nPtsGraph[I];
        if (out != old)
          changed = true;
      }
      nPtsGraph[I] = out;
    }

    if (changed) {
      for (auto si = succ_begin(BB), se = succ_end(BB); si != se; ++si) {
        BasicBlock *succ = *si;
        if (std::find(worklist.begin(), worklist.end(), succ) == worklist.end())
          worklist.push_back(succ);
      }
    }
  } // worklist
}

void FuncAnalysis::handleGEPConstant(const ConstantExpr *ce, PtsGraph &in) {
  NodeIndex gepIndex = nodeFactory.getValueNodeFor(ce);
  NodeIndex srcNode = nodeFactory.getValueNodeFor(ce->getOperand(0));
  int64_t offset = getGEPOffset(ce, DL);
  int fieldNum = 0;
  if (offset < 0) {
    // update the out pts graph instead of the in
    //<= functon: acpi_ns_validate_handle: %7 = icmp eq i8* %6, getelementptr
    //(i8, i8* null, i64 -1)
    if (ce->getOperand(0)->isNullValue())
      return;

    // if(cast<PointerType>(ce->getType())->getElementType()->isIntegerTy(8))
    // fieldNum = handleContainerOf(ce, offset, srcNode, in);
    //  XXX: negative encoded as unsigned
  } else {
    fieldNum = offsetToFieldNum(ce->getOperand(0)->stripPointerCasts(), offset,
                                DL, &Ctx->structAnalyzer, M);
  }

  for (auto n : in[srcNode]) {
    // assert(nodeFactory.isObjectNode(n) && "src ptr does not point to obj
    // node"); if (!nodeFactory.isObjectNode(n))
    if (n > nodeFactory.getConstantIntNode() &&
        !nodeFactory.isUnOrArrObjNode(n))
      in[gepIndex].insert(n + fieldNum);
    else
      in[gepIndex].insert(n);
  }
}

PtsGraph FuncAnalysis::processInstruction(Instruction *I, PtsGraph &in) {

  // handle constant GEPExpr oprands here
  for (auto const &opr : I->operands()) {
    const Value *v = opr.get();
    if (const ConstantExpr *ce = dyn_cast<ConstantExpr>(v)) {
      if (ce->getOpcode() == Instruction::GetElementPtr) {
        NodeIndex gepIndex = nodeFactory.getValueNodeFor(ce);
        NodeIndex srcNode = nodeFactory.getValueNodeFor(ce->getOperand(0));
        int64_t offset = getGEPOffset(ce, DL);
        int fieldNum = 0;
        if (offset < 0) {
          // update the out pts graph instead of the in
          //<= functon: acpi_ns_validate_handle: %7 = icmp eq i8* %6,
          //getelementptr (i8, i8* null, i64 -1)
          if (ce->getOperand(0)->isNullValue())
            continue;

          if (I->getType()->isPointerTy() &&
              I->getType()->getPointerElementType()->isIntegerTy(8))
            fieldNum = handleContainerOf(I, offset, srcNode, in);
          // XXX: negative encoded as unsigned
        } else {
          fieldNum = offsetToFieldNum(ce->getOperand(0)->stripPointerCasts(),
                                      offset, DL, &Ctx->structAnalyzer, M);
        }

        // in[gepIndex].insert(nodeFactory.getObjNodeForGEPExpr(gepIndex));

        for (auto n : in[srcNode]) {
          // assert(nodeFactory.isObjectNode(n) && "src ptr does not point to
          // obj node"); if (!nodeFactory.isObjectNode(n))
          if (n > nodeFactory.getConstantIntNode() &&
              !nodeFactory.isUnOrArrObjNode(n))
            in[gepIndex].insert(n + fieldNum);
          else
            in[gepIndex].insert(n);
        }
      }
      if (ce->getOpcode() == Instruction::BitCast) {
        if (const ConstantExpr *cep =
                dyn_cast<ConstantExpr>(ce->getOperand(0))) {
          if (cep->getOpcode() == Instruction::GetElementPtr) {
            handleGEPConstant(cep, in);
          }
        }
      }
    }
  }

  PtsGraph out = in;
  switch (I->getOpcode()) {
  case Instruction::Alloca: {
    NodeIndex valNode = nodeFactory.getValueNodeFor(I);
    assert(valNode != AndersNodeFactory::InvalidIndex &&
           "Failed to find alloca value node");
    assert(I->getType()->isPointerTy());
    createNodeForPointerVal(I, I->getType(), valNode, out);
    // cannot call the function createNodeForPointer, because the allocated
    break;
  }
  case Instruction::Load: {
    if (I->getType()->isPointerTy()) {
      // val = load from op
      NodeIndex ptrNode = nodeFactory.getValueNodeFor(I->getOperand(0));
      assert(ptrNode != AndersNodeFactory::InvalidIndex &&
             "Failed to find load operand node");
      NodeIndex valNode = nodeFactory.getValueNodeFor(I);
      assert(valNode != AndersNodeFactory::InvalidIndex &&
             "Failed to find load value node");

      for (auto i : in[ptrNode]) {
#ifdef DBG_LOAD
        OP << i << ":\n";
        for (auto item : in[i]) {
          OP << "item " << item << ", ";
        }
        OP << "\n";
#endif
        out[valNode].insert(in[i].begin(), in[i].end());
        // if the ins is a pointer but it points to nothing, then we put
        // universal ptr node into it.
        if (out[valNode].isEmpty()) {
          out[valNode].insert(nodeFactory.getUniversalPtrNode());
        }
        if (nodeFactory.isUnOrArrObjNode(i)) {
          nodeFactory.setUnOrArrObjNode(valNode);
        }
      }
    }
    break;
  }
  case Instruction::Store: {
    NodeIndex dstNode = nodeFactory.getValueNodeFor(I->getOperand(1));
    assert(dstNode != AndersNodeFactory::InvalidIndex &&
           "Failed to find store dst node");
    NodeIndex srcNode = nodeFactory.getValueNodeFor(I->getOperand(0));
    if (srcNode == AndersNodeFactory::InvalidIndex) {
      // If we hit the store insturction like below, it is
      // possible that the global variable (init_user_ns) is
      // an extern variable. Hope this does not happen,
      // otherwise we need to assume the worst case :-(

      // 1. store %struct.user_namespace* @init_user_ns,
      // %struct.user_namespace**  %from.addr.i, align 8
      // 2. @init_user_ns = external global %struct.user_namespace
      errs() << "Failed to find store src node for " << *I << "\n";
      break;
    }
    // assert(srcIndex != AndersNodeFactory::InvalidIndex && "Failed to find
    // store src node");

    if (in.find(srcNode) != in.end()) {
      for (auto i : in[dstNode]) {
        if (i <= nodeFactory.getConstantIntNode())
          continue;
        out[i] = in[srcNode];
        if (nodeFactory.isArgNode(srcNode)) {
          nodeFactory.setArgNode(i);
        }
      }
    }

    break;
  }
  case Instruction::GetElementPtr: {
    // dstNode = gep srcNode offset
    NodeIndex srcNode = nodeFactory.getValueNodeFor(I->getOperand(0));
    assert(srcNode != AndersNodeFactory::InvalidIndex &&
           "Failed to find gep src node");
    NodeIndex dstNode = nodeFactory.getValueNodeFor(I);
    assert(dstNode != AndersNodeFactory::InvalidIndex &&
           "Failed to find gep dst node");
    const GEPOperator *gepValue = dyn_cast<GEPOperator>(I);
    assert(gepValue != NULL && "getGEPOffset receives a non-gep value!");
    int64_t offset = getGEPOffset(I, DL);
    llvm::Type *srcType = I->getType();

    const Type *type = srcType->getPointerElementType();
    // An array is considered a single variable of its type.
    while (const ArrayType *arrayType = dyn_cast<ArrayType>(type))
      type = arrayType->getPointerElementType();

    // sequential gep inst, function pci_msi_prepare, we treat it as one field
    bool innerele = false;
    for (auto obj : in[srcNode]) {
      if (offset < 0 && in[obj].getSize() == 1) {
        for (auto inner : in[obj]) {
          if (inner <= nodeFactory.getConstantIntNode())
            innerele = true;
        }
      }
    }

    if (innerele) {
      out[dstNode] = in[srcNode];
      break;
    }
    int fieldNum = 0;
    bool onlyUnion = true;
    if (offset < 0) {
      bool special = true;
      for (auto obj : out[srcNode]) {
        if (obj <= nodeFactory.getConstantIntNode()) {
          out[dstNode].insert(obj);
          continue;
        } else {
          special = false;
        }
        // update the out pts graph instead of the in
      }
      if (!special) {

        if (I->getType()->getPointerElementType()->isIntegerTy(8))
          fieldNum = handleContainerOf(I, offset, srcNode, out);
      }
      // XXX: negative encoded as unsigned
    } else {
      for (auto obj : out[srcNode]) {
        if (obj <= nodeFactory.getConstantIntNode()) {
          out[dstNode].insert(obj);
          continue;
        }
        if (!nodeFactory.isUnOrArrObjNode(obj)) {
          onlyUnion = false;
        }
      }
      if (!onlyUnion) {
        fieldNum = offsetToFieldNum(I->getOperand(0)->stripPointerCasts(),
                                    offset, DL, &Ctx->structAnalyzer, M);
      }
      // check if the offset of union, but I remember I change the function
      // offsetToFieldNum, so this could be removed?

      for (auto n : in[srcNode]) {
#ifdef checkunion
        if (nodeFactory.isUnionObjectNode(n)) {
          out[dstNode].insert(n);
          continue;
        }
#endif
        if (n > nodeFactory.getConstantIntNode()) {
          if (fieldNum <= nodeFactory.getObjectSize(n)) {
            unsigned bound = nodeFactory.getObjectBound(n);
            if (n + fieldNum < bound)
              out[dstNode].insert(n + fieldNum);
            else
              out[dstNode].insert(n);
          } else
            out[dstNode].insert(nodeFactory.getUniversalPtrNode());
        } else
          out[dstNode].insert(n);
      }
    }
    break;
  }
  case Instruction::PHI: {
    const PHINode *PHI = cast<PHINode>(I);
    if (PHI->getType()->isPointerTy()) {
      NodeIndex dstNode = nodeFactory.getValueNodeFor(PHI);
      assert(dstNode != AndersNodeFactory::InvalidIndex &&
             "Failed to find phi dst node");
      for (unsigned i = 0, e = PHI->getNumIncomingValues(); i != e; ++i) {
        NodeIndex srcNode =
            nodeFactory.getValueNodeFor(PHI->getIncomingValue(i));
        assert(srcNode != AndersNodeFactory::InvalidIndex &&
               "Failed to find phi src node");
        if (in.find(srcNode) != in.end())
          out[dstNode].insert(in[srcNode].begin(), in[srcNode].end());
      }
    }
    break;
  }
  case Instruction::BitCast: {
    NodeIndex dstNode = nodeFactory.getValueNodeFor(I);
    assert(dstNode != AndersNodeFactory::InvalidIndex &&
           "Failed to find bitcast dst node");
    NodeIndex srcNode = nodeFactory.getValueNodeFor(I->getOperand(0));
    assert(srcNode != AndersNodeFactory::InvalidIndex &&
           "Failed to find bitcast src node");
    assert(I->getType()->isPointerTy());
    Type *T = I->getType()->getPointerElementType();
    Type *srcTy = I->getOperand(0)->getType()->getPointerElementType();

    if (nodeFactory.isUnOrArrObjNode(srcNode)) {
      nodeFactory.setUnOrArrObjNode(dstNode);
    }
    // do not cast a union obj
    bool unionType = false;
    for (auto obj : out[srcNode]) {
      if (obj <= nodeFactory.getConstantIntNode())
        continue;
      unsigned objSize = nodeFactory.getObjectSize(obj);
      if (objSize == 1 && nodeFactory.isUnOrArrObjNode(obj)) {
        out[dstNode] = in[srcNode];
        unionType = true;
        break;
      }
    }
    if (unionType) {
      for (auto obj : out[dstNode]) {
        if (obj <= nodeFactory.getConstantIntNode())
          continue;
        nodeFactory.setUnOrArrObjNode(obj);
      }
      break;
    }

    if (StructType *srcST = dyn_cast<StructType>(srcTy)) {
      if (!srcST->isLiteral() && srcST->getStructName().startswith("union")) {
        out[dstNode] = in[srcNode];
        break;
      }
    }
    if (GetElementPtrInst *GEPI =
            dyn_cast<GetElementPtrInst>(I->getOperand(0))) {
      if (getGEPOffset(GEPI, DL) != 0) {
        out[dstNode] = in[srcNode];
        break;
      }
    }
    if (StructType *ST = dyn_cast<StructType>(T)) {
      if (ST->isOpaque()) {
        out[dstNode] = out[srcNode];
        break;
      }
      const StructInfo *stInfo = Ctx->structAnalyzer.getStructInfo(ST, M);
      assert(stInfo != NULL && "Failed to find stinfo");

      unsigned stSize = stInfo->getExpandedSize();
      // disable assertion: retrieve_io_and_region_from_bio
      for (NodeIndex obj : in[srcNode]) {
        if (obj <= nodeFactory.getConstantIntNode())
          continue;
        // special case: gep and then cast, <=Function get_dx_countlimit
        if (!nodeFactory.isObjectNode(obj))
          continue;
        unsigned objSize = nodeFactory.getObjectSize(obj);
        if (stSize > objSize) {
#if 0
                        OP << "BitCast: " << *I << ":"
                           << " dst size " << stSize
                           << " larger than src size " << objSize << "\n";
#endif
          NodeIndex newObj = extendObjectSize(obj, ST, out);
          unsigned newOffset = nodeFactory.getObjectOffset(newObj);
          // if the object is pointed in the middle, then load will cause
          // problems, we need an assertion to promise the object is not read.
          if (newObj == obj) {
            for (User *U : I->users()) {
              if (Instruction *Use = dyn_cast<Instruction>(U)) {

                if (isa<LoadInst>(Use)) {
                  OP << "Use: " << *Use << "\n";
                  OP << "Warning: Casting struct get used.\n";
                }
                if (isa<GetElementPtrInst>(Use)) {
                  GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(Use);
                  int64_t off = getGEPOffset(GEPI, DL);
                  if (off == 0)
                    continue;
                  for (User *GEPU : Use->users()) {
                    if (Instruction *GepUse = dyn_cast<Instruction>(GEPU)) {
                      if (isa<LoadInst>(GepUse)) {
                        OP << "Use: " << *GepUse << "\n";
                        OP << "Warning: Casting struct get used.\n";
                      }
                    }
                  }
                }
              }
            }
          }
          if (newObj != obj && nodeFactory.isArgNode(obj)) {
            for (unsigned i = 0; i < stSize; i++) {
              nodeFactory.setArgNode(newObj - newOffset + i);
            }
          }
          if (newObj != obj && nodeFactory.isHeapNode(obj)) {
            for (unsigned i = 0; i < stSize; i++) {
              nodeFactory.setHeapNode(newObj - newOffset + i);
            }
          }
        }
        // the obj could not be the first element of the obj
        else {
          unsigned newOffset = nodeFactory.getObjectOffset(obj);
          for (unsigned i = 0; i < stSize; i++) {
            if (stInfo->isFieldUnion(i))
              nodeFactory.setUnOrArrObjNode(obj - newOffset + i);
          }
          if (nodeFactory.isHeapNode(obj)) {
            for (unsigned i = 0; i < stSize; i++) {
              nodeFactory.setHeapNode(obj - newOffset + i);
            }
          }
        }
      }
    }
    // since we may have updated the pts graph, we need to use the out instead
    // of in
    out[dstNode] = out[srcNode];
    break;
  }
  case Instruction::Trunc: {
    if (I->getType()->isPointerTy()) {
      NodeIndex srcNode = nodeFactory.getValueNodeFor(I->getOperand(0));
      assert(srcNode != AndersNodeFactory::InvalidIndex &&
             "Failed to find trunc src node");
      NodeIndex dstNode = nodeFactory.getValueNodeFor(I);
      assert(dstNode != AndersNodeFactory::InvalidIndex &&
             "Failed to find trunc dst node");
      if (in.find(srcNode) != in.end())
        out[dstNode] = in[srcNode];
    }
    break;
  }
  case Instruction::IntToPtr: {
    // Get the node index for dst
    NodeIndex dstNode = nodeFactory.getValueNodeFor(I);
    NodeIndex srcNode = nodeFactory.getValueNodeFor(I->getOperand(0));
    assert(dstNode != AndersNodeFactory::InvalidIndex &&
           "Failed to find inttoptr dst node");
    assert(srcNode != AndersNodeFactory::InvalidIndex &&
           "Failed to find inttoptr src node");

    if (nodeFactory.isUnOrArrObjNode(srcNode)) {
      nodeFactory.setUnOrArrObjNode(dstNode);
    }
    // ptr to int to ptr
    if (in.find(srcNode) != in.end() && !in[srcNode].isEmpty())
      out[dstNode] = in[srcNode];
    else
      out[dstNode].insert(nodeFactory.getUniversalPtrNode());
    break;
  }
    // FIXME point arithmetic through int
  case Instruction::PtrToInt: {
    // Get the node index for dst
    NodeIndex dstNode = nodeFactory.getValueNodeFor(I);
    NodeIndex srcNode = nodeFactory.getValueNodeFor(I->getOperand(0));
    assert(dstNode != AndersNodeFactory::InvalidIndex &&
           "Failed to find inttoptr dst node");
    assert(srcNode != AndersNodeFactory::InvalidIndex &&
           "Failed to find inttoptr src node");

    if (nodeFactory.isUnOrArrObjNode(srcNode)) {
      nodeFactory.setUnOrArrObjNode(dstNode);
    }
    out[dstNode] = in[srcNode];
    break;
  }
  case Instruction::Select: {
    out = in;
    if (I->getType()->isPointerTy()) {
      NodeIndex dstNode = nodeFactory.getValueNodeFor(I);
      assert(dstNode != AndersNodeFactory::InvalidIndex &&
             "Failed to find select dst node");

      NodeIndex srcNode = nodeFactory.getValueNodeFor(I->getOperand(1));
      assert(srcNode != AndersNodeFactory::InvalidIndex &&
             "Failed to find select src node 1");
      if (in.find(srcNode) != in.end())
        out[dstNode].insert(in[srcNode].begin(), in[srcNode].end());

      srcNode = nodeFactory.getValueNodeFor(I->getOperand(2));
      assert(srcNode != AndersNodeFactory::InvalidIndex &&
             "Failed to find select src node 2");
      if (in.find(srcNode) != in.end())
        out[dstNode].insert(in[srcNode].begin(), in[srcNode].end());
    }
    break;
  }
  case Instruction::VAArg: {
    if (I->getType()->isPointerTy()) {
      NodeIndex dstNode = nodeFactory.getValueNodeFor(I);
      assert(dstNode != AndersNodeFactory::InvalidIndex &&
             "Failed to find va_arg dst node");
      NodeIndex vaNode =
          nodeFactory.getVarargNodeFor(I->getParent()->getParent());
      assert(vaNode != AndersNodeFactory::InvalidIndex &&
             "Failed to find va_arg node");
      if (in.find(vaNode) != in.end())
        out[dstNode] = in[vaNode];
    }
    break;
  }
  case Instruction::Call: {
    CallInst *CI = dyn_cast<CallInst>(I);
    if (CI->getCalledFunction()) {
      std::string CFN = CI->getCalledFunction()->getName().str();
    }
    if (I->getType()->isPointerTy()) {
      NodeIndex dstNode = nodeFactory.getValueNodeFor(I);
      assert(dstNode != AndersNodeFactory::InvalidIndex &&
             "Failed to find call dst node");
      out[dstNode].clear();
      bool init = false;
      if (CI->getCalledFunction()) {
        std::string CFN = getScopeName(CI->getCalledFunction());
      }
      for (auto *func : Ctx->Callees[CI]) {
        StringRef fName = func->getName();
        if (!init) {
          createNodeForPointerVal(I, I->getType(), dstNode, out);
          init = true;
        }

        if (Ctx->HeapAllocFuncs.count(fName.str())) {
          NodeIndex dstNode = nodeFactory.getValueNodeFor(I);
          for (auto obj : out[dstNode]) {
            if (obj <= nodeFactory.getConstantIntNode())
              continue;
            unsigned objSize = nodeFactory.getObjectSize(obj);
            for (unsigned i = 0; i < objSize; i++) {
              nodeFactory.setHeapNode(obj + i);
            }
          }
        } else if (Ctx->CopyFuncs.count(fName.str())) {
          if (CI->arg_size() < 3)
            break;
          if (!CI->getArgOperand(0)->getType()->isPointerTy() ||
              !CI->getArgOperand(1)->getType()->isPointerTy())
            break;
          NodeIndex dstIndex =
              nodeFactory.getValueNodeFor(CI->getArgOperand(0));
          NodeIndex srcIndex =
              nodeFactory.getValueNodeFor(CI->getArgOperand(1));
          if (CI->getArgOperand(0)->getType()->isPointerTy() &&
              CI->getArgOperand(1)->getType()->isPointerTy()) {
            for (auto srcObj : out[srcIndex]) {
              for (auto dstObj : out[dstIndex]) {
                out[dstObj].insert(out[srcObj].begin(), out[srcObj].end());
              }
            }
          }
        }
      }
    }
    break;
  }
  case Instruction::Ret: {
    if (I->getNumOperands() != 0) {
      Value *RV = I->getOperand(0);
      NodeIndex valNode = nodeFactory.getValueNodeFor(RV);
      assert(valNode != AndersNodeFactory::InvalidIndex &&
             "Failed to find call return value node");
      if (RV->getType()->isPointerTy()) {
        NodeIndex retNode = nodeFactory.getReturnNodeFor(F);
        assert(retNode != AndersNodeFactory::InvalidIndex &&
               "Failed to find call return node");
        out[retNode] = in[valNode];
      } else {
        if (!isa<ConstantInt>(RV)) {
          auto itr = in.find(valNode);
        }
      }
    }
    break;
  }
  default: {
    if (I->getType()->isPointerTy())
      OP << "pointer-related inst not handled!" << *I << "\n";
    out = in;
    break;
  }
  } // switch(I->getOpCode())
  return out;
}

void FuncAnalysis::unionPtsGraph(PtsGraph &pg, const PtsGraph &other) {
  for (auto itr2 = other.begin(), end2 = other.end(); itr2 != end2; ++itr2) {
    auto itr1 = pg.find(itr2->first);
    if (itr1 != pg.end())
      itr1->second.insert(itr2->second.begin(), itr2->second.end());
    else
      pg.insert(std::make_pair(itr2->first, itr2->second));
  }
}

void FuncAnalysis::initSummary() {
  OP << "inside initSummary:\n";
  fSummary.relatedBC.insert(F->getParent()->getName().str());
  // retValue and argValue node are created in: createInitNodes
  Instruction *beginIns = &(F->front().front());
  Instruction *endIns = &(F->back().back());
  // 1-1. create return value node and argument value node
  NodeIndex sumRetNode = fSummary.sumNodeFactory.getValueNodeFor(F);
  NodeIndex retNode = nodeFactory.getReturnNodeFor(F);
  // 1-2 create return object node
  if (F->getReturnType()->isVoidTy()) {
    fSummary.sumPtsGraph[sumRetNode].clear();
  } else {
    // create node and build the pts relations of return object
    Type *type = F->getReturnType();
    Type *eleType = NULL;
    unsigned objSize = 0;
    if (!type->isVoidTy()) {
      if (type->isPointerTy()) {
        eleType = type->getPointerElementType();
        while (const ArrayType *arrayType = dyn_cast<ArrayType>(eleType))
          eleType = arrayType->getPointerElementType();
        if (const StructType *structType = dyn_cast<StructType>(eleType)) {
          if (structType->isOpaque()) {
            objSize = 1;
          } else if (!structType->isLiteral() &&
                     structType->getStructName().startswith("union")) {
            objSize = 1;
          } else {
            const StructInfo *stInfo =
                Ctx->structAnalyzer.getStructInfo(structType, M);
            objSize = stInfo->getExpandedSize();
          }
        } else {
          objSize = 1;
        }
      }
    }
    fSummary.setRetSize(objSize);
    NodeIndex sumRetObjNode = fSummary.sumNodeFactory.createObjectNode(F);
    fSummary.setRetSize(objSize);

    for (auto obj : nPtsGraph[endIns][retNode]) {
      if (obj <= nodeFactory.getConstantIntNode())
        continue;
      if (fSummary.sumNodeFactory.getObjectNodeFor(F) !=
          AndersNodeFactory::InvalidIndex)
        break;
      // OP<<"1fSumamry.retSize = "<<fSummary.getRetSize()<<"\n";
      //         OP<<"sumRetNode = "<<sumRetNode<<", objSize = "<<objSize<<"\n";
      // OP << "sumRetObjNode = " << sumRetObjNode << "\n";
      //     OP<<"offset = "<<nodeFactory.getObjectOffset(obj)<<"\n";
      if (nodeFactory.getObjectOffset(obj) == 0) {
        fSummary.sumPtsGraph[sumRetNode].insert(sumRetObjNode);
        fSummary.setRetOffset(0);
      }
      for (unsigned i = 1; i < objSize; i++) {
        NodeIndex sumObjNode =
            fSummary.sumNodeFactory.createObjectNode(sumRetObjNode, i);
        if (i == nodeFactory.getObjectOffset(obj)) {
          fSummary.sumPtsGraph[sumRetNode].insert(sumObjNode);
          fSummary.setRetOffset(i);
        }
      }
    }
  }
  // OP<<"create object node for args.\n";
  // 1-3 create argument object node
  int argNo = 0;
  for (auto const &a : F->args()) {
    const Argument *arg = &a;
    NodeIndex argNode = nodeFactory.getValueNodeFor(arg);
    NodeIndex sumArgNode = fSummary.sumNodeFactory.getValueNodeFor(arg);
    // OP<<"argNo = "<<argNo<<", sumArgNode = "<<sumArgNode<<"\n";

    fSummary.args[sumArgNode].setNodeIndex(sumArgNode);
    fSummary.args[sumArgNode].setNodeArgNo(argNo);
    // OP<<"sumArgNode = "<<sumArgNode<<", argNode = "<<argNode<<"\n";;
    // create node for object, set the sumPts information
    // create the array for update and requirement of the function
    for (auto obj : nPtsGraph[endIns][argNode]) {
      // OP<<"obj = "<<obj<<"\n";
      if (obj <= nodeFactory.getConstantIntNode())
        continue;
      unsigned objSize = nodeFactory.getObjectSize(obj);
      NodeIndex sumArgObjNode = fSummary.sumNodeFactory.getObjectNodeFor(arg);
      // OP<<"obj index = "<<fSummary.sumNodeFactory.getObjectNodeFor(arg)<<",
      // objSize = "<<objSize<<"\n";;
      if (sumArgObjNode != AndersNodeFactory::InvalidIndex)
        break;
      fSummary.args[argNo].setObjSize(objSize);
      sumArgObjNode = fSummary.sumNodeFactory.createObjectNode(arg);
      // OP<<"sumArgObjNode = "<<sumArgObjNode<<"\n";
      if (nodeFactory.getObjectOffset(obj) == 0) {
        fSummary.sumPtsGraph[sumArgNode].insert(sumArgObjNode);
        fSummary.args[sumArgObjNode].setNodeArgNo(argNo);
        fSummary.args[sumArgObjNode].setOffset(0);
      }
      for (unsigned i = 1; i < objSize; i++) {
        NodeIndex objNode =
            fSummary.sumNodeFactory.createObjectNode(sumArgObjNode, i);
        fSummary.args[objNode].setNodeArgNo(argNo);
        if (i == nodeFactory.getObjectOffset(obj)) {
          fSummary.sumPtsGraph[sumArgNode].insert(objNode);
          fSummary.args[argNo].setOffset(i);
        }
      }
    }
    argNo++;
  }
#ifdef _PRINT_SUMNODEFACT
  OP << "sumNodeFactory:\n";
  for (int i = 0; i < fSummary.sumNodeFactory.getNumNodes(); i++)
    fSummary.sumNodeFactory.dumpNode(i);
  OP << "sumPtsGraph:\n";
  for (auto i : fSummary.sumPtsGraph) {
    OP << "ptr: " << i.first << "->";
    for (auto o : i.second) {
      OP << o << "\n";
    }
  }
#endif
  // 2. create the summary array for both update and requirments
  unsigned numNodes = fSummary.sumNodeFactory.getNumNodes();
  fSummary.noNodes = numNodes;
  fSummary.reqVec.resize(numNodes);    //= new int[numNodes];
  fSummary.updateVec.resize(numNodes); //= new int[numNodes];
  for (unsigned i = 0; i < numNodes; i++) {
    fSummary.reqVec.at(i) = QualifierState::Unknown;
    fSummary.updateVec.at(i) = QualifierState::Unknown;
  }
}

// given old object node and new struct info, extend the object size
// return the new object node
NodeIndex FuncAnalysis::extendObjectSize(NodeIndex oldObj,
                                         const StructType *stType,
                                         PtsGraph &ptsGraph) {
  // FIXME: assuming oldObj is the base <= function:
  // acpi_dev_filter_resource_type
  // disable assertion:  function: acpi_dev_filter_resource_type
  // assert(nodeFactory.getObjectOffset(oldObj) == 0);
  if (nodeFactory.getObjectOffset(oldObj) != 0)
    return oldObj;

  const Value *val = nodeFactory.getValueForNode(oldObj);
  assert(val != nullptr && "Failed to find value of node");
  NodeIndex valNode = nodeFactory.getValueNodeFor(val);

  // before creating new obj, remove the old ptr->obj
  auto itr = ptsGraph.find(valNode);
  // function tcp_prune_ofo_queue
  assert(itr != ptsGraph.end());
  if (!itr->second.has(oldObj)) {
    OP << "valNode does not own the oldObj.";
  }
  itr->second.reset(oldObj);
  nodeFactory.removeNodeForObject(val);

  // create new obj
  NodeIndex newObj = processStruct(val, stType, valNode, ptsGraph);
  // set ArgNode:
  if (nodeFactory.isArgNode(oldObj)) {
    unsigned stSize = nodeFactory.getObjectSize(newObj);
    unsigned newOffset = nodeFactory.getObjectOffset(newObj);
    for (unsigned i = 0; i < stSize; i++)
      nodeFactory.setArgNode(newObj - newOffset + i);
  }
  // update ptr2set. all the pointers to oldObj should to new Obj
  updateObjectNode(oldObj, newObj, ptsGraph);

  return newObj;
}

// given the old object node, old size, and the new object node
// replace all point-to info to the new node
// including the value to object node mapping
void FuncAnalysis::updateObjectNode(NodeIndex oldObj, NodeIndex newObj,
                                    PtsGraph &ptsGraph) {
  unsigned offset = nodeFactory.getObjectOffset(oldObj);
  NodeIndex baseObj = nodeFactory.getOffsetObjectNode(oldObj, -(int)offset);
  unsigned size = nodeFactory.getObjectSize(oldObj);
  // well, really expensive op
  // use explicit iterator for updating
  for (auto itr = ptsGraph.begin(), end = ptsGraph.end(); itr != end; ++itr) {
    AndersPtsSet temp = itr->second;
    // in case modification will break iteration
    for (NodeIndex obj : temp) {
      // if(nodeFactory.getOffset(obj) == 0)
      // continue;
      if (obj >= baseObj && obj < (baseObj + size)) {
        itr->second.reset(obj);
        itr->second.insert(newObj + (obj - baseObj));
      }
    }
  }
}
void QualifierAnalysis::printWarnForRec(std::vector<llvm::Function *> &scc) {
#ifdef WRITEJSON
  std::ofstream jfile;
  if (Ctx->incAnalysis) {
    jfile.open(Ctx->jsonInc, std::ios::app);
  } else {
    jfile.open(Ctx->jsonfile, std::ios::app);
  }
#endif
  for (llvm::Function *F : scc) {
    auto warnSet = Ctx->fToWarns[F];
    for (auto item : warnSet) {
#ifdef WRITEJSON
      // print the related bc files:
      jfile << item.first << "\n";
      jfile << item.second << "\n";
#endif
    }
  }
#ifdef WRITEJSON
  jfile.close();
#endif
  Ctx->fToWarns.clear();
}
int FuncAnalysis::handleContainerOf(const Instruction *I, int64_t offset,
                                    NodeIndex srcNode, PtsGraph &ptsGraph) {
  assert(I->getType()->isPointerTy() &&
         "passing non-pointer type to handleContainerOf");
  assert(I->getType()->getPointerElementType()->isIntegerTy(8) &&
         "handleContainerOf can only handle i8*");
  assert(offset < 0 && "handleContainerOf can only handle negative offset");
  NodeIndex gepIndex = nodeFactory.getValueNodeFor(I);
  // for each obj the pointer can point-to
  // hopefully there'll only be one obj <= too young too naive, function:
  // drm_vma_offset_lookup_locked, store nullptr or arg to it.
  // assert(ptsGraph[srcNode].getSize() == 1);
  for (NodeIndex obj : ptsGraph[srcNode]) {
    // if src obj special, ptr arithmetic is meaningless
    if (obj <= nodeFactory.getConstantIntNode())
      return 0;

    // find the allocated type
    const Value *allocObj = nodeFactory.getValueForNode(obj);
    const Type *allocType = allocObj->getType();

    // 1. check if allocated type is okay
    assert(allocType->isPointerTy());
    const StructType *stType =
        dyn_cast<StructType>(allocType->getPointerElementType());
    if (!stType) {
      OP << "!stType, the precise cannot be promised.\n";
      return 0;
    }
    // assert(stType && "handleContainerOf can only handle struct type");
    const StructInfo *stInfo = Ctx->structAnalyzer.getStructInfo(stType, M);
    assert(stInfo != NULL && "structInfoMap should have info for st!");
    // find the size offset of src ptr
    // int64_t should be large enough for unsigned
    // srcField: the obj field from the current obj
    int64_t srcField = nodeFactory.getObjectOffset(obj);
    // szieOffset: the offset from the current obj
    if (srcField >= stInfo->getExpandedSize())
      continue;
    int64_t sizeOffset = stInfo->getFieldOffset(srcField);
    // check if the size offset of src ptr is large enough to handle the
    // negative offset
    if (sizeOffset + offset >= 0) {
      // FIXME: assume only one obj
      int64_t dstField =
          offsetToFieldNum(I->getOperand(0)->stripPointerCasts(),
                           sizeOffset + offset, DL, &Ctx->structAnalyzer, M);
      //? src - dst?
      return (int)(dstField - srcField);
    }

    // 2. If the allocated type is not large enough to handle the offset,
    //    create a new one and update the ptr2set
    // get real type from later bitcast
    const Type *realType = nullptr;
    const Value *castInst = nullptr;
    for (const auto *U : I->users()) {
      if (isa<BitCastInst>(U)) {
        realType = U->getType();
        castInst = U;
        // break;
        // #ifdef nonsense
        // comment for function queue_attr_show in blk-sysfs, var %24.
        if (isa<GetElementPtrInst>(U->getOperand(0))) {
          ptsGraph[gepIndex] = ptsGraph[srcNode];
          OP << "gep inst, the precise cannot be promised.\n";
          return 0;
        }
        // #endif
      }
    }
    // assertion disabled: function dm_per_bio_data
    if (!realType) {
      OP << "Failed to find the dst type for container_of\n";
      return 0;
    }
    // assert(realType && "Failed to find the dst type for container_of");
    assert(realType->isPointerTy());
    // special case: function dequeue_func
    const StructType *rstType =
        dyn_cast<StructType>(realType->getPointerElementType());

    if (!rstType) {
      OP << "handleContainerOf can only handle struct type\n";
      return 0;
    }
    // assert(rstType && "handleContainerOf can only handle struct type");
    //
    //  this is the tricky part, we need to find the field number inside the
    //  larger struct
    const StructInfo *rstInfo = Ctx->structAnalyzer.getStructInfo(rstType, M);
    assert(rstInfo != nullptr && "structInfoMap should have info for rst!");

    // double check the container type?
    //  FIXME: assuming one container_of at a time
    //  fix stInfo if srcField is not 0
    bool found_container = false;
    std::set<Type *> elementTypes = stInfo->getElementType(srcField);
    for (Type *t : elementTypes) {
      if (StructType *est = dyn_cast<StructType>(t)) {

        const StructInfo *estInfo = Ctx->structAnalyzer.getStructInfo(est, M);
        assert(estInfo != nullptr && "structInfoMap should have info for est!");
        if (estInfo->getContainer(rstInfo->getRealType(), -offset)) {
          found_container = true;
          break;
        }
      }
    }
    if (!found_container) {
      OP << "found_container fails, the precise cannot be promised.";
      return 0;
    }

    // okay, now we have find the correct container
    // create the new obj, using the castinst as base
    // This is an ugly hack, because we don't really have an original obj

    NodeIndex valNode = nodeFactory.getValueNodeFor(castInst);
    assert(valNode != AndersNodeFactory::InvalidIndex &&
           "Failed to find castinst node");
    NodeIndex castObjNode = processStruct(castInst, rstType, valNode, ptsGraph);
    ptsGraph[gepIndex].insert(castObjNode);
    // next, update the ptr2set to the new object
    unsigned subField =
        offsetToFieldNum(castInst, -offset, DL, &Ctx->structAnalyzer, M);
    NodeIndex newObjNode =
        nodeFactory.getOffsetObjectNode(castObjNode, subField);
    updateObjectNode(obj, newObjNode, ptsGraph);

    if (nodeFactory.isArgNode(obj)) {
      unsigned newSize = nodeFactory.getObjectSize(newObjNode);
      unsigned newOffset = nodeFactory.getObjectOffset(newObjNode);
      for (unsigned i = 0; i < newSize; i++) {
        nodeFactory.setArgNode(newObjNode - newOffset + i);
      }
    }
    // update the value -> obj map
    nodeFactory.updateNodeForObject(allocObj, newObjNode);
    // finally, return the offset/fieldNum
    return -subField;
  } // end for all obj in ptr2set
  return 0;
}

static bool isCastingCompatibleType(Type *T1, Type *T2) {
  if (T1->isPointerTy()) {
    if (!T2->isPointerTy())
      return false;

    Type *ElT1 = T1->getPointerElementType();
    Type *ElT2 = T2->getPointerElementType();
    // assume "void *" and "char *" are equivalent to any pointer type
    if (ElT1->isIntegerTy(8) /*|| ElT2->isIntegerTy(8)*/)
      return true;
    // if(ElT1->isStructTy())
    // return true;

    return isCastingCompatibleType(ElT1, ElT2);
  } else if (T1->isArrayTy()) {
    if (!T2->isArrayTy())
      return false;
    Type *ElT1 = T1->getArrayElementType();
    Type *ElT2 = T2->getArrayElementType();
    return isCastingCompatibleType(ElT1, ElT1);
  } else if (T1->isIntegerTy()) {
    // assume pointer can be cased to the address space size
    if (T2->isPointerTy() &&
        T1->getIntegerBitWidth() == T2->getPointerAddressSpace())
      return true;

    // assume all integer type are compatible
    if (T2->isIntegerTy())
      return true;
    else
      return false;
  } else if (T1->isStructTy()) {
    return true;
  } else {
    errs() << "Unhandled Types:" << *T1 << " :: " << *T2 << "\n";
    return T1->getTypeID() == T2->getTypeID();
  }
}
void FuncAnalysis::addTerminationBB(llvm::BasicBlock *bb) {
  int count = 0;
  llvm::BasicBlock *cur = bb;
  std::unordered_set<llvm::BasicBlock *> visit;
  visit.insert(cur);
  for (auto si = succ_begin(cur), se = succ_end(cur); si != se; ++si)
    count++;
  while (count == 1) {
    count = 0;
    for (auto si = succ_begin(cur), se = succ_end(cur); si != se; ++si) {
      if (visit.count(*si))
        continue;
      llvm::BasicBlock *temp = *si;
      visit.insert(cur);
      count++;
      if (temp->size() > 1) {
        count = 0;
        break;
      }
      cur = temp;
    }
  }
  terminationBB.insert(cur);
}
void Tarjan::dfs(llvm::Function *F) {
  dfn[F] = low[F] = ts++;
  depVisit.insert(F);
  Stack.push(F);
  inStack[F] = true;
  for (auto *iter : depCG[F]) {
    llvm::Function *func = iter;
    if (!depVisit.count(func)) {
      dfs(func);
      if (low[func] < low[F]) {
        low[F] = low[func];
      }
    } else {
      if (dfn[func] < low[F] && inStack[func]) {
        low[F] = dfn[func];
      }
    }
  }

  llvm::Function *temp = NULL;
  sc.clear();
  if (dfn[F] == low[F]) {
    do {
      temp = Stack.top();
      sc.push_back(temp);
      Stack.pop();
      inStack[temp] = false;
    } while (temp != F);

    SCC.push_back(sc);
    int n = sc.size();
    if (n > biggest)
      biggest = n;
    if (sc.size() == 1 && depCG[sc.back()].count(sc.back())) {
      rec++;
    }
    sccSize += sc.size();
  }
}

void Tarjan::getSCC(std::vector<std::vector<llvm::Function *>> &ans) {
  int count = 0;
  for (auto *iter : funcs) {
    llvm::Function *cur = iter;
    if (depVisit.count(cur))
      continue;
    dfs(cur);
    count++;
    // OP<<"count = "<<count<<", function :"<<cur->getName().str()<<"\n";
  }
  ans = SCC;
  OP << "partition size : " << count << ", sccSize = " << sccSize
     << ", rec size = " << rec << ", biggest = " << biggest << "\n";
}
