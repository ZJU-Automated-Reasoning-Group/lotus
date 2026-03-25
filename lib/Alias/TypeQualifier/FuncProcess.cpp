#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Value.h"

#include "Alias/TypeQualifier/Annotation.h"
#include "Alias/TypeQualifier/Helper.h"
#include "Alias/TypeQualifier/QualifierAnalysis.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace llvm;
void FuncAnalysis::processInitFuncs(Instruction *I, llvm::Function *Callee,
                                    bool init, QualifierArray &in,
                                    QualifierArray &out) {
  CallInst *CI = dyn_cast<CallInst>(I);
  Value *dst = CI->getArgOperand(0);
  NodeIndex dstIndex = nodeFactory.getValueNodeFor(dst);
  QualifierState qualiSrc = QualifierState::Initialized;
  std::set<const llvm::Value *> reqVisit;
  if (out.at(dstIndex) == QualifierState::Unknown) {
    if (dstIndex > nodeFactory.getConstantIntNode())
      setReqFor(I, dst, out, reqVisit);
    out.at(dstIndex) = QualifierState::Initialized;
  }
  if (out.at(dstIndex) == QualifierState::Uninitialized)
    qualiSrc = QualifierState::Uninitialized;

  if (Callee->getName().str() == "llvm.va_start") {
    for (auto obj : nPtsGraph[I][dstIndex]) {
      if (obj <= nodeFactory.getConstantIntNode())
        continue;
      int objSize = nodeFactory.getObjectSize(obj);
      for (int i = 0; i < objSize; i++)
        out.at(obj + i) = qualiSrc;
    }
    return;
  }

  Value *src = CI->getArgOperand(1);
  Value *size = CI->getArgOperand(2);
  NodeIndex srcIndex = nodeFactory.getValueNodeFor(src);
  NodeIndex sizeIndex = nodeFactory.getValueNodeFor(size);
  // 0.set the qualifer req for args
  if (out.at(srcIndex) == QualifierState::Unknown) {
    if (srcIndex > nodeFactory.getConstantIntNode())
      setReqFor(I, src, out, reqVisit);
    out.at(srcIndex) = QualifierState::Initialized;
  }
  if (out.at(sizeIndex) == QualifierState::Unknown) {
    if (sizeIndex > nodeFactory.getConstantIntNode())
      setReqFor(I, size, out, reqVisit);
    out.at(sizeIndex) = QualifierState::Initialized;
  }
  qualiSrc = QualifierState::Initialized;
  // 1. set the qualifier of target to initialized
  if (out.at(srcIndex) == QualifierState::Uninitialized || out.at(sizeIndex) == QualifierState::Uninitialized) {
    qualiSrc = QualifierState::Uninitialized;
  }
  // look the real type for memset.
  const Type *type = dst->getType()->getPointerElementType();
  while (const ArrayType *arrayType = dyn_cast<ArrayType>(type))
    type = arrayType->getPointerElementType();
  // Now construct the pointer and memory object variable
  // It depends on whether the type of this variable is a struct or not
  if (const StructType *structType = dyn_cast<StructType>(type)) {
    const StructInfo *stInfo = Ctx->structAnalyzer.getStructInfo(structType, M);
    uint64_t stSize = stInfo->getExpandedSize();
    for (auto obj : nPtsGraph[I][dstIndex]) {
      for (int i = 0; i < stSize; i++) {
        out.at(obj + i) = qualiSrc;
      }
    }
  } else {
    for (auto obj : nPtsGraph[I][dstIndex]) {
      if (obj <= nodeFactory.getConstantIntNode())
        continue;
      out.at(obj) = qualiSrc;
    }
  }
}
void FuncAnalysis::processCopyFuncs(Instruction *I, llvm::Function *Callee,
                                    bool init, QualifierArray &in,
                                    QualifierArray &out) {
  unsigned numNodes = nodeFactory.getNumNodes();
  CallInst *CI = dyn_cast<CallInst>(I);
  if (CI->arg_size() < 3)
    return;
  std::set<const llvm::Value *> reqVisit;

  Value *dst = CI->getArgOperand(0);
  Value *src = CI->getArgOperand(1);
  Value *size = CI->getArgOperand(2);
  NodeIndex dstIndex = nodeFactory.getValueNodeFor(dst);
  NodeIndex srcIndex = nodeFactory.getValueNodeFor(src);
  NodeIndex sizeIndex = nodeFactory.getValueNodeFor(size);

  // 0.set the qualifer req for args
  if (out.at(srcIndex) == QualifierState::Unknown) {
    setReqFor(I, src, out, reqVisit);
    out.at(srcIndex) = QualifierState::Initialized;
  }
  if (out.at(dstIndex) == QualifierState::Unknown) {
    setReqFor(I, dst, out, reqVisit);
    out.at(dstIndex) = QualifierState::Initialized;
  }
  if (out.at(sizeIndex) == QualifierState::Unknown) {
    setReqFor(I, size, out, reqVisit);
    out.at(sizeIndex) = QualifierState::Initialized;
  }
  if (!dst->getType()->isPointerTy() || !src->getType()->isPointerTy())
    return;

  if (out.at(srcIndex) == QualifierState::Uninitialized || out.at(sizeIndex) == QualifierState::Uninitialized) {
    out.at(dstIndex) = QualifierState::Uninitialized;
    return;
  }
  uint64_t objSize = 0;
  // 1. If we copy an arg node to a heap pbj or an unknown pointer obj,
  // we need to set the requirement for arg
  for (auto obj : nPtsGraph[I][dstIndex]) {
    // copy to the heap
    if (obj < nodeFactory.getNullPtrNode() || nodeFactory.isHeapNode(obj)) {
      for (auto srcObj : nPtsGraph[I][srcIndex]) {
        if (srcObj <= nodeFactory.getConstantIntNode())
          continue;
        unsigned srcObjOffset = nodeFactory.getObjectOffset(srcObj);
        unsigned srcObjSize = nodeFactory.getObjectSize(srcObj);
        for (unsigned i = srcObjOffset; i < srcObjSize - srcObjOffset; i++) {
          if (nQualiArray[I].at(srcObj + i) == QualifierState::Unknown &&
              nRequiredIn[I].at(srcObj + i) == RequirednessState::Required) {
            out.at(srcObj + i) = QualifierState::Initialized;
          }
        }
      }
    }
    if (nodeFactory.isArgNode(obj)) {
      for (auto srcObj : nPtsGraph[I][srcIndex]) {
        if (srcObj <= nodeFactory.getConstantIntNode())
          continue;
        unsigned srcObjOffset = nodeFactory.getObjectOffset(srcObj);
        unsigned srcObjSize = nodeFactory.getObjectSize(srcObj);
        for (unsigned i = srcObjOffset; i < srcObjSize - srcObjOffset; i++) {
          if (in.at(srcObj + i) == QualifierState::Unknown) {
            relatedNode[obj + i].insert(srcObj + i);
            relatedNode[obj + i].insert(relatedNode[srcObj + i].begin(),
                                        relatedNode[srcObj + i].end());
          }
        }
      }
    }
  }
  // 2. Identify the object size to be copied.
  for (auto obj : nPtsGraph[I][dstIndex]) {
    if (obj <= nodeFactory.getConstantIntNode())
      continue;
    if (objSize > 0)
      objSize = ((objSize < nodeFactory.getObjectSize(obj))
                     ? objSize
                     : nodeFactory.getObjectSize(obj));
    else
      objSize = nodeFactory.getObjectSize(obj);
    if (nodeFactory.isUnOrArrObjNode(obj)) {
      objSize = 1;
      break;
    }
    // copy from middle, the field pointer.
    if (objSize > nodeFactory.getObjectOffset(obj) &&
        nodeFactory.getObjectOffset(obj) != 0) {
      objSize =
          nodeFactory.getObjectSize(obj) - nodeFactory.getObjectOffset(obj);
    }
  }
  for (auto srcObj : nPtsGraph[I][srcIndex]) {
    if (srcObj <= nodeFactory.getConstantIntNode())
      continue;
    if (nodeFactory.isUnOrArrObjNode(srcObj)) {
      objSize = 1;
      break;
    }
    unsigned srcObjOffset = nodeFactory.getObjectOffset(srcObj);
    objSize = ((objSize < nodeFactory.getObjectSize(srcObj) - srcObjOffset)
                   ? objSize
                   : nodeFactory.getObjectSize(srcObj) - srcObjOffset);
  }
  // consider casting to i8* before copy, this case may be included into the
  // previous calculate the object size case.
  if (BitCastInst *BCI = dyn_cast<BitCastInst>(src)) {
    const Type *type = I->getOperand(0)->getType()->getPointerElementType();

    while (const ArrayType *arrayType = dyn_cast<ArrayType>(type))
      type = arrayType->getPointerElementType();
    if (const StructType *structType = dyn_cast<StructType>(type)) {
      if (!structType->isOpaque()) {
        const StructInfo *stInfo =
            Ctx->structAnalyzer.getStructInfo(structType, F->getParent());
        objSize = stInfo->getExpandedSize();
      }
      // OP<<"4objSize = "<<objSize<<"\n";
      if (!structType->isLiteral() &&
          structType->getStructName().startswith("union"))
        objSize = 1;
    }
  }
  if (out.at(dstIndex) == QualifierState::Initialized) {
    // Now construct the pointer and memory object variable
    // It depends on whether the type of this variable is a struct or not
    bool init = false;
    for (auto obj : nPtsGraph[I][dstIndex]) {
      // unsigned objSize = nodeFactory.getObjectSize(obj);
      // considering memcpy to universal ptr node
      if (obj <= nodeFactory.getConstantIntNode())
        continue;
      for (auto srcObj : nPtsGraph[I][srcIndex]) {
        if (obj < nodeFactory.getNullPtrNode()) {
          if (out.at(srcObj) == QualifierState::Unknown &&
              nRequiredIn[I].at(srcObj) == RequirednessState::Required) {
            out.at(srcObj) = QualifierState::Initialized;
          }
        }
        // unsigned objSize = nodeFactory.getObjectSize(srcObj);
        for (int i = 0; i < objSize; i++) {
          // considering memcpy from universal ptr node
          QualifierState srcQuali = QualifierState::Initialized;
          if (srcObj <= nodeFactory.getConstantIntNode()) {
            srcQuali = QualifierState::Initialized;
          } else {
            srcQuali = out.at(srcObj + i);
            if (srcQuali == QualifierState::Unknown) {
              relatedNode[obj + i].insert(srcObj + i);
              for (auto aa : nAAMap[I][srcObj]) {
                relatedNode[obj + i].insert(relatedNode[aa].begin(),
                                            relatedNode[aa].end());
              }
#ifdef _RELATED
              OP << "related node for " << obj + i << "\n";
              for (auto item : relatedNode[obj + i])
                OP << item << "/";
              OP << "\n";
#endif
            }
          }
          if (obj + i >= numNodes)
            break;
          if (!init || srcQuali == QualifierState::Initialized) {
            out.at(obj + i) = srcQuali;
            init = true;
          } else {
            out.at(obj + i) = std::min(out.at(obj + i), srcQuali);
          }
        }
      }
    }
  }
}
void FuncAnalysis::processTransferFuncs(Instruction *I, llvm::Function *Callee,
                                        bool init, QualifierArray &in,
                                        QualifierArray &out) {
  CallInst *CI = dyn_cast<CallInst>(I);
  // 1. copy the qualifier inference from src to dst
  Value *dst = CI->getArgOperand(0);
  Value *src = CI->getArgOperand(1);
  Value *size = CI->getArgOperand(2);
  NodeIndex dstIndex = nodeFactory.getValueNodeFor(dst);
  NodeIndex srcIndex = nodeFactory.getValueNodeFor(src);
  NodeIndex sizeIndex = nodeFactory.getValueNodeFor(size);
  std::set<const llvm::Value *> reqVisit;
  // 0.set the qualifer req for args
  if (out.at(srcIndex) == QualifierState::Unknown) {
    out.at(srcIndex) = QualifierState::Initialized;
    setReqFor(I, src, out, reqVisit);
  }
  if (out.at(dstIndex) == QualifierState::Unknown) {
    out.at(dstIndex) = QualifierState::Initialized;
    setReqFor(I, dst, out, reqVisit);
  }
  if (out.at(sizeIndex) == QualifierState::Unknown) {
    out.at(sizeIndex) = QualifierState::Initialized;
    setReqFor(I, size, out, reqVisit);
  }

  const Type *type = dst->getType()->getPointerElementType();
  while (const ArrayType *arrayType = dyn_cast<ArrayType>(type))
    type = arrayType->getPointerElementType();

  // 1. copy the data from src to dst
  if (out.at(srcIndex) != QualifierState::Initialized || out.at(sizeIndex) != QualifierState::Initialized) {
    out.at(dstIndex) = QualifierState::Uninitialized;
    return;
  }
  if (out.at(dstIndex) == QualifierState::Initialized) {
    bool init = false;
    // Now construct the pointer and memory object variable
    // It depends on whether the type of this variable is a struct or not
    if (const StructType *structType = dyn_cast<StructType>(type)) {
      const StructInfo *stInfo =
          Ctx->structAnalyzer.getStructInfo(structType, M);
      uint64_t stSize = stInfo->getExpandedSize();
      for (auto obj : nPtsGraph[I][dstIndex]) {
        for (auto srcObj : nPtsGraph[I][srcIndex]) {
          for (int i = 0; i < stSize; i++) {
            if (out.at(srcObj + i) == QualifierState::Unknown &&
                nRequiredIn[I].at(srcObj + i) == RequirednessState::Required) {
              out.at(srcObj + i) = QualifierState::Initialized;
            }
            QualifierState srcQuali = QualifierState::Initialized;
            if (srcObj <= nodeFactory.getConstantIntNode()) {
              srcQuali = QualifierState::Initialized;
            } else {
              srcQuali = in[srcObj + i];
            }
            if (!init || srcQuali == QualifierState::Uninitialized) {
              out.at(obj + i) = srcQuali;
              init = true;
            } else {
              out.at(obj + i) = std::min(out.at(obj + i), srcQuali);
            }
          }
        }
      }
    } else {
      for (auto obj : nPtsGraph[I][dstIndex]) {
        for (auto srcObj : nPtsGraph[I][srcIndex]) {
          if (out.at(srcObj) == QualifierState::Unknown) {
            reqVisit.clear();
            setReqFor(I, src, out, reqVisit);
            out.at(srcObj) = QualifierState::Initialized;
          }
          QualifierState srcQuali = QualifierState::Initialized;
          if (srcObj <= nodeFactory.getConstantIntNode()) {
            srcQuali = QualifierState::Initialized;
          } else {
            srcQuali = in[srcObj];
          }
          if (!init || srcQuali == QualifierState::Uninitialized) {
            out.at(obj) = srcQuali;
            init = true;
          } else {
            out.at(obj) = std::min(out.at(obj), srcQuali);
          }
        }
      }
    }
  }
}
void FuncAnalysis::processFuncs(llvm::Instruction *I, llvm::Function *Callee,
                                bool init, QualifierArray &in,
                                QualifierArray &out) {

  OP << "inside processFuncs:\n";
  CallInst *CI = dyn_cast<CallInst>(I);
#ifdef RET_CHECK
  // If there's a retCheck, then we initilaize all the arguments
  bool retCheck = false;
  for (User *U : I->users()) {
    Instruction *Ins = dyn_cast<Instruction>(U);
    if (ICmpInst *ICI = dyn_cast<ICmpInst>(Ins)) {
      Value *cmp = ICI->getOperand(1);
      if (ConstantPointerNull *cpn = dyn_cast<ConstantPointerNull>(cmp)) {
        retCheck = true;
      } else if (ConstantInt *cmp = dyn_cast<ConstantInt>(cmp)) {
        retCheck = true;
      }
    }
    if (StoreInst *SI = dyn_cast<StoreInst>(Ins)) {
      for (User *UU : SI->getPointerOperand()->users()) {
        if (ConstantExpr *CE = dyn_cast<ConstantExpr>(UU))
          continue;
        Instruction *UseIns = dyn_cast<Instruction>(UU);
        if (LoadInst *LI = dyn_cast<LoadInst>(UseIns)) {
          for (User *LIU : LI->users()) {
            Instruction *LUseIns = dyn_cast<Instruction>(LIU);
            // OP<<"LUseIns = "<<*LUseIns<<"\n";
            if (ICmpInst *ICI = dyn_cast<ICmpInst>(LUseIns)) {
              Value *cmp = ICI->getOperand(1);
              NodeIndex cmpIndex = nodeFactory.getValueNodeFor(cmp);
              if (cmpIndex > nodeFactory.getConstantIntNode())
                continue;
              if (cmpIndex == nodeFactory.getConstantIntNode()) {
                retCheck = true;
              } else if (ConstantPointerNull *cpn =
                             dyn_cast<ConstantPointerNull>(cmp)) {
                retCheck = true;
              } else if (ConstantInt *cmp = dyn_cast<ConstantInt>(cmp)) {
                retCheck = true;
              }
            }
          }
        }
      }
    }
  }
#endif
  if (Callee->getName().str().find("printf") != std::string::npos) {
    for (int argNo = 0; argNo < (int)CI->arg_size(); argNo++) {
      NodeIndex argNode = nodeFactory.getValueNodeFor(CI->getArgOperand(argNo));
      for (auto obj : nPtsGraph[I][argNode]) {
        if (obj <= nodeFactory.getConstantIntNode())
          continue;
        unsigned objSize = nodeFactory.getObjectSize(obj);
        unsigned objOffset = nodeFactory.getObjectOffset(obj);
        for (unsigned i = 0; i < objSize; i++) {
          if (nQualiArray[I].at(obj - objOffset + i) == QualifierState::Unknown) {
            out.at(obj - objOffset + i) = QualifierState::Initialized;

            // DFS(I, obj - objOffset + i);
          }
        }
      }
    }
    // update return value to be: initialized:
    NodeIndex retNode = nodeFactory.getValueNodeFor(I);
    out.at(retNode) = QualifierState::Initialized;
    for (auto retObj : nPtsGraph[I][retNode]) {
      if (retObj <= nodeFactory.getConstantIntNode())
        continue;
      unsigned retObjSize = nodeFactory.getObjectSize(retObj);
      unsigned objOffset = nodeFactory.getObjectOffset(retObj);
      for (unsigned i = 0; i < retObjSize; i++) {
        out.at(retObj - objOffset + i) = QualifierState::Initialized;
      }
    }
    return;
  }
  if (Callee->empty()) {
    auto FIter = Ctx->Funcs.find(Callee->getName().str());
    if (FIter != Ctx->Funcs.end()) {
      Callee = FIter->second;
    }
  }

  // only update the arguments and the return value from Callees, req
  // propagation is done later in summarizeFuncs().
#ifdef PRINT_SUMF
  OP << "Functions in Ctx->FSummaries: \n";
  for (auto item : Ctx->FSummaries) {
    OP << "--Addr : " << item.first << ", name: " << item.first->getName().str()
       << "\n";
  }
#endif
  if (Callee == NULL || Ctx->FSummaries.find(Callee) == Ctx->FSummaries.end()) {
    OP << "Function not Found.\n";
    return;
  }

  // Callee's summary
  std::set<const llvm::Value *> reqVisit;
// Insert the callee's related BC to the caller
#ifdef _LINK
  for (auto item : Ctx->FSummaries[Callee].relatedBC) {
    fSummary.relatedBC.insert(item);
  }
#endif
  std::vector<const Argument *> calleeArgs;
  for (Function::arg_iterator itr = Callee->arg_begin(),
                              ite = Callee->arg_end();
       itr != ite; ++itr) {
    const Argument *arg = &*itr;
    calleeArgs.push_back(arg);
  }
  int sumNumNodes = Ctx->FSummaries[Callee].noNodes;
  if (Ctx->FSummaries[Callee].isEmpty()) {
    OP << "Summary is empty\n";
    return;
  }
  // update the argument and propagate the requirement
  for (int argNo = 0; argNo < (int)CI->arg_size(); argNo++) {

    NodeIndex argNode = nodeFactory.getValueNodeFor(CI->getArgOperand(argNo));
    // Deal with VarArg function
    if (argNo >= calleeArgs.size()) {
      if (out.at(argNode) == QualifierState::Unknown) {
        out.at(argNode) = QualifierState::Initialized;
        reqVisit.clear();
        setReqFor(I, CI->getArgOperand(argNo), out, reqVisit);
      }
      for (auto obj : nPtsGraph[I][argNode]) {
        if (obj <= nodeFactory.getConstantIntNode())
          continue;
        unsigned objSize = nodeFactory.getObjectSize(obj);
        unsigned objOffset = nodeFactory.getObjectOffset(obj);
        for (unsigned i = 0; i < objSize; i++) {
          if (nQualiArray[I].at(obj - objOffset + i) == QualifierState::Unknown) {
            out.at(obj - objOffset + i) = QualifierState::Initialized;
          }
        }
      }
      continue;
    }
    NodeIndex sumArgNode =
        Ctx->FSummaries[Callee].sumNodeFactory.getValueNodeFor(
            calleeArgs.at(argNo));
    if (sumArgNode == AndersNodeFactory::InvalidIndex)
      continue;
    // Check the pointer itself
    if (Ctx->FSummaries[Callee].reqVec.at(sumArgNode) == QualifierState::Initialized) {
      if (out.at(argNode) == QualifierState::Unknown) {
        reqVisit.clear();
        setReqFor(I, CI->getArgOperand(argNo), out, reqVisit);
      }
    }
    unsigned numNodes = nodeFactory.getNumNodes();
    auto sumPtsIt = Ctx->FSummaries[Callee].sumPtsGraph.find(sumArgNode);
    if (sumPtsIt == Ctx->FSummaries[Callee].sumPtsGraph.end())
      continue;
    for (auto sumObj : sumPtsIt->second) {
      unsigned sumObjSize = Ctx->FSummaries[Callee].args[argNo].getObjSize();
      unsigned sumObjOffset = Ctx->FSummaries[Callee].args[argNo].getOffset();
      for (auto obj : nPtsGraph[I][argNode]) {
        if (obj <= nodeFactory.getConstantIntNode())
          continue;
        // Deal with the union object
        unsigned objSize = nodeFactory.getObjectSize(obj);
        unsigned objOffset = nodeFactory.getObjectOffset(obj);
        unsigned checkSize =
            std::min(objSize - objOffset, sumObjSize - sumObjOffset);
        if (nodeFactory.isUnOrArrObjNode(obj)) {
          bool init = false;
          for (unsigned i = 0; i < checkSize; i++) {
            if (Ctx->FSummaries[Callee].updateVec[sumObj + i] != QualifierState::Unknown) {
              if (!init) {
                if (out.at(obj) >= Ctx->FSummaries[Callee].reqVec[sumObj + i]) {
                  out.at(obj) = Ctx->FSummaries[Callee].updateVec[sumObj + i];
                  init = true;
                }
              } else {
                if (out.at(obj) >= Ctx->FSummaries[Callee].reqVec[sumObj + i]) {
                  out.at(obj) =
                      std::min(out.at(obj),
                               Ctx->FSummaries[Callee].updateVec[sumObj + i]);
                }
              }
            }
#ifdef ListProp
            if (Ctx->FSummaries[Callee].updateVec[sumObj + i] != QualifierState::Initialized) {
              nodeFactory.setWL(
                  obj, Ctx->FSummaries[Callee].args[sumObj + i].getWhiteList());
              nodeFactory.setBL(
                  obj, Ctx->FSummaries[Callee].args[sumObj + i].getBlackList());
            }
#endif
#ifdef RM
            if (Ctx->FSummaries[Callee].reqVec.at(sumObj + i) == QualifierState::Initialized) {
              if (out.at(obj) == QualifierState::Unknown) {
                out.at(obj) = QualifierState::Initialized;
                // DFS(I, obj);
              }
            }
#endif
          }
        } else {
          // update the arg->obj from the offset till end
          for (unsigned i = 0; i < checkSize; i++) {
            if (obj + i >= numNodes)
              break;
#ifdef ListProp
            if (Ctx->FSummaries[Callee].updateVec[sumObj + i] != QualifierState::Initialized) {
              nodeFactory.setWL(
                  obj + i,
                  Ctx->FSummaries[Callee].args[sumObj + i].getWhiteList());
              nodeFactory.setBL(
                  obj + i,
                  Ctx->FSummaries[Callee].args[sumObj + i].getBlackList());
            }
#endif

            if (Ctx->FSummaries[Callee].updateVec[sumObj + i] != QualifierState::Unknown) {
              if (out.at(obj + i) >=
                  Ctx->FSummaries[Callee].reqVec[sumObj + i]) {
                out.at(obj + i) = Ctx->FSummaries[Callee].updateVec[sumObj + i];
              }
            } else {
              std::set<NodeIndex> sumRSet =
                  Ctx->FSummaries[Callee].args[sumObj + i].getRelatedArgs();
              if (sumRSet.empty())
                continue;
              bool init = false;
              for (auto sumRitem : sumRSet) {
                if (Ctx->FSummaries[Callee].args.size() == 0)
                  break;
                int argNoCurr =
                    Ctx->FSummaries[Callee].args[sumRitem].getNodeArgNo();
                if (argNoCurr >= (int)CI->arg_size()) {
                  continue;
                }
                NodeIndex argNoIndex =
                    nodeFactory.getValueNodeFor(CI->getArgOperand(argNoCurr));
                if (argNoIndex == AndersNodeFactory::InvalidIndex) {
                  continue;
                }
                if (nodeFactory.isObjectNode(argNoIndex) &&
                    nodeFactory.getObjectSize(argNoIndex) < i)
                  continue;
                if (argNoIndex + i >= nodeFactory.getNumNodes())
                  continue;
                if (out.at(argNoIndex + i) == QualifierState::Unknown) {
                  relatedNode[obj + i].insert(
                      relatedNode[argNoIndex + i].begin(),
                      relatedNode[argNoIndex + i].end());
                }
                if (!init) {
                  out.at(obj + i) = out.at(argNoIndex + i);
                  init = true;
                } else {
                  out.at(obj + i) =
                      std::min(out.at(obj + i), out.at(argNoIndex + i));
                }
              }
            }
#ifdef RM
            if (Ctx->FSummaries[Callee].reqVec.at(sumObj + i) == QualifierState::Initialized) {
              if (out.at(obj + i) == QualifierState::Unknown) {
                out.at(obj + i) = QualifierState::Initialized;
                // DFS(I, obj + i);
              }
            }
#endif
          }
        }
        // update the rest object if arg point to the middle of the obj
        if (sumObjOffset > 0 && objOffset > 0) {
          if (sumObjOffset < objOffset)
            continue;

          for (unsigned i = objOffset; i > 0; i--) {
            if (Ctx->FSummaries[Callee].updateVec[sumObj - i] != QualifierState::Unknown) {
              if (out.at(obj - i) >=
                  Ctx->FSummaries[Callee].reqVec[sumObj - i]) {
                out.at(obj - i) = Ctx->FSummaries[Callee].updateVec[sumObj - i];
              }
#ifdef RM
              if (Ctx->FSummaries[Callee].reqVec.at(sumObj - i) == QualifierState::Initialized) {
                out.at(obj - i) = QualifierState::Initialized;
                // DFS(I, obj - i);
              }
#endif
            }
#ifdef ListProp
            if (Ctx->FSummaries[Callee].updateVec[sumObj - i] != QualifierState::Initialized) {
              nodeFactory.setWL(
                  obj - i,
                  Ctx->FSummaries[Callee].args[sumObj - i].getWhiteList());
              nodeFactory.setBL(
                  obj - i,
                  Ctx->FSummaries[Callee].args[sumObj - i].getBlackList());
            }
#endif
          }
        }
      }
    }
  }
  // update the return value and object
  if (I->getType()->isVoidTy()) {
    OP << "return void.\n";
    return;
  }
  NodeIndex retNode = nodeFactory.getValueNodeFor(I);
  NodeIndex sumRetNode = Ctx->FSummaries[Callee].getRetNodes();
  out.at(retNode) = Ctx->FSummaries[Callee].updateVec[sumRetNode];
  // context-sensitivity
  std::set<NodeIndex> sumRSet =
      Ctx->FSummaries[Callee].args[sumRetNode].getRelatedArgs();
  if (out.at(retNode) != QualifierState::Initialized) {
    nodeFactory.setWL(retNode,
                      Ctx->FSummaries[Callee].args[sumRetNode].getWhiteList());
    nodeFactory.setBL(retNode,
                      Ctx->FSummaries[Callee].args[sumRetNode].getBlackList());
  }
  if (!sumRSet.empty()) {
    bool init = false;
    for (auto sumRitem : sumRSet) {
      int argNoCurr = Ctx->FSummaries[Callee].args[sumRitem].getNodeArgNo();
      if (argNoCurr >= (int)CI->arg_size() ||
          isa<Function>(CI->getArgOperand(argNoCurr)))
        continue;
      NodeIndex argNoIndex =
          nodeFactory.getObjectNodeFor(CI->getArgOperand(argNoCurr));
      int argOffset = Ctx->FSummaries[Callee].args[sumRitem].getOffset();
      if (argNoIndex == AndersNodeFactory::InvalidIndex && argOffset == 0) {
        argNoIndex = nodeFactory.getValueNodeFor(CI->getArgOperand(argNoCurr));
      }
      if (argNoIndex == AndersNodeFactory::InvalidIndex)
        continue;
      if (out.at(argNoIndex + argOffset) == QualifierState::Unknown) {
        relatedNode[retNode].insert(relatedNode[argNoIndex + argOffset].begin(),
                                    relatedNode[argNoIndex + argOffset].end());
      }
      if (!init) {
        out.at(retNode) = out.at(argNoIndex + argOffset);
        init = true;
      } else {
        out.at(retNode) =
            std::min(out.at(retNode), out.at(argNoIndex + argOffset));
      }
    }
  }
  //
  for (auto sumRetObj : Ctx->FSummaries[Callee].sumPtsGraph[sumRetNode]) {
    unsigned sumRetSize = Ctx->FSummaries[Callee].getRetSize();
    unsigned sumRetOffset = Ctx->FSummaries[Callee].getRetOffset();

    for (auto retObj : nPtsGraph[I][retNode]) {
      if (retObj <= nodeFactory.getConstantIntNode())
        continue;
      unsigned retObjSize = nodeFactory.getObjectSize(retObj);
      unsigned retObjOffset = nodeFactory.getObjectOffset(retObj);
      unsigned checkSize =
          std::min(sumRetSize - sumRetOffset, retObjSize - retObjOffset);

      for (unsigned i = 0; i < checkSize; i++) {
        if (Ctx->FSummaries[Callee].updateVec[sumRetObj + i] != QualifierState::Unknown) {
          out.at(retObj + i) = Ctx->FSummaries[Callee].updateVec[sumRetObj + i];
        } else {
          std::set<NodeIndex> sumRSet =
              Ctx->FSummaries[Callee].args[sumRetObj + i].getRelatedArgs();
          if (sumRSet.empty())
            continue;
          bool init = false;
          for (auto sumRitem : sumRSet) {
            int argNoCurr =
                Ctx->FSummaries[Callee].args[sumRitem].getNodeArgNo();
            NodeIndex argNoIndex =
                nodeFactory.getObjectNodeFor(CI->getArgOperand(argNoCurr));
            if (argNoIndex == AndersNodeFactory::InvalidIndex)
              continue;
            if (out.at(argNoIndex + i) == QualifierState::Unknown) {
              relatedNode[retObj + i].insert(
                  relatedNode[argNoIndex + i].begin(),
                  relatedNode[argNoIndex + i].end());
            }
            if (!init) {
              out.at(retObj + i) = out.at(argNoIndex + i);
              init = true;
            } else {
              out.at(retObj + i) =
                  std::min(out.at(retObj + i), out.at(argNoIndex + i));
            }
          }
        }
        if (out.at(retObj + i) != QualifierState::Initialized) {

          nodeFactory.setWL(
              retObj + i,
              Ctx->FSummaries[Callee].args[sumRetObj + i].getWhiteList());
          nodeFactory.setBL(
              retObj + i,
              Ctx->FSummaries[Callee].args[sumRetObj + i].getBlackList());
        }
      }
    }
  }
}

void FuncAnalysis::checkCopyFuncs(llvm::Instruction *I,
                                  llvm::Function *Callee) {
  CallInst *CI = dyn_cast<CallInst>(I);
  std::string warningTy = "OTHER";
  if (CI->arg_size() < 3)
    return;
  std::set<const Instruction *> visit;
  Value *dst = CI->getArgOperand(0);
  Value *src = CI->getArgOperand(1);
  NodeIndex dstIndex = nodeFactory.getValueNodeFor(CI->getArgOperand(0));
  NodeIndex srcIndex = nodeFactory.getValueNodeFor(CI->getArgOperand(1));
  NodeIndex sizeIndex = nodeFactory.getValueNodeFor(CI->getArgOperand(2));
  if (!dst->getType()->isPointerTy() || !src->getType()->isPointerTy())
    return;
  if (nQualiArray[I].at(dstIndex) != QualifierState::Initialized) {
    insertUninit(I, dstIndex, uninitArg);
  }

  if (nQualiArray[I].at(dstIndex) == QualifierState::Uninitialized) {
    if (!warningReported(I, dstIndex)) {
#ifdef PRINTWARN
      OP << "***********Warning: argument check fails @ argument 0: dst "
            "pointer not initialized\n";
      OP << "[trace] In function @" << Callee->getName().str()
         << " Instruction:" << *I << "\n";
#endif
      visit.clear();
      warningTy = warningTy = eToS[getWType(I->getOperand(0)->getType())];
      printRelatedBB(dstIndex, I, visit, warningTy, 0);
      warningSet.insert(dstIndex);
    }
  }
  if (nQualiArray[I].at(srcIndex) != QualifierState::Initialized) {
    insertUninit(I, srcIndex, uninitArg);
  }

  if (nQualiArray[I].at(srcIndex) == QualifierState::Uninitialized) {
    if (!warningReported(I, srcIndex)) {
      insertUninit(I, srcIndex, uninitArg);
#ifdef PRINTWARN
      OP << "***********Warning: argument check fails @ argument 1: src "
            "pointer not initialized\n";
      OP << "[trace] In function @" << Callee->getName().str()
         << " Instruction:" << *I << "\n";
#endif
      visit.clear();
      warningTy = eToS[getWType(I->getOperand(1)->getType())];
      printRelatedBB(srcIndex, I, visit, warningTy, 1);
      warningSet.insert(srcIndex);
    }
  }
  // check the src object, if the dst obj is heap node then the qualifier should
  // be initialized:
  for (auto dstObj : nPtsGraph[I][dstIndex]) {
    if (dstObj < nodeFactory.getNullPtrNode() ||
        nodeFactory.isHeapNode(dstObj)) {
      for (auto obj : nPtsGraph[I][srcIndex]) {
        if (obj <= nodeFactory.getConstantIntNode())
          continue;
        unsigned objSize = nodeFactory.getObjectSize(obj);
        // copy from middle, the field pointer.
        if (nodeFactory.getObjectOffset(obj) != 0) {
          objSize = objSize - nodeFactory.getObjectOffset(obj);
        }
        for (int i = 0; i < objSize; i++) {
          if (nQualiArray[I].at(obj + i) != QualifierState::Initialized) {
            insertUninit(I, obj + i, uninitArg);
          }
          if (nQualiArray[I].at(obj + i) == QualifierState::Uninitialized) {
            if (warningReported(I, obj + i))
              continue;
#ifdef PRINTWARN
            OP << "***********Warning: argument check fails @ argument 1: field"
               << i << " not initialized\n";
            OP << "[trace] In function @" << Callee->getName().str()
               << " Instruction:" << *I << "\n";
            OP << "obj + i = " << obj + i
               << ", qualifier =" << nQualiArray[I].at(obj + i) << "\n";
#endif
            visit.clear();
            printRelatedBB(obj + i, I, visit, "DATA", 1, i);
            warningSet.insert(obj + i);
          }
        }
      }
    } else {
    }
  }

  if (nQualiArray[I].at(sizeIndex) == QualifierState::Uninitialized) {
    if (nQualiArray[I].at(sizeIndex) != QualifierState::Initialized) {
      insertUninit(I, sizeIndex, uninitArg);
    }
    if (!warningReported(I, sizeIndex)) {
#ifdef PRINTWARN
      OP << "***********Warning: argument check fails @ argument 3: size not "
            "initialized\n";
      OP << "[trace] In function @" << Callee->getName().str()
         << " Instruction:" << *I << "\n";
#endif
      visit.clear();
      printRelatedBB(sizeIndex, I, visit, "DATA", 3);
      warningSet.insert(sizeIndex);
    }
  }
}
void FuncAnalysis::checkTransferFuncs(llvm::Instruction *I,
                                      llvm::Function *Callee) {
  CallInst *CI = dyn_cast<CallInst>(I);
  NodeIndex dstIndex = nodeFactory.getValueNodeFor(CI->getArgOperand(0));
  NodeIndex srcIndex = nodeFactory.getValueNodeFor(CI->getArgOperand(1));
  NodeIndex sizeIndex = nodeFactory.getValueNodeFor(CI->getArgOperand(2));
  std::set<const Instruction *> visit;
  std::string warningTy = "OTHER";
  if (nQualiArray[I].at(srcIndex) == QualifierState::Uninitialized) {
    if (nQualiArray[I].at(srcIndex) == QualifierState::Uninitialized) {
      insertUninit(I, srcIndex, uninitArg);
    }

    if (!warningReported(I, srcIndex)) {
#ifdef PRINTWARN
      OP << "***********Arg Warning: argument check fails @ argument 1: src "
            "pointer not initialized\n";
      OP << "[trace] In function @" << Callee->getName().str()
         << " Instruction:" << *I << "\n";
#endif
      visit.clear();
      warningTy = eToS[getWType(I->getOperand(0)->getType())];
      printRelatedBB(srcIndex, I, visit, warningTy, 1);
      warningSet.insert(srcIndex);
    }
  }

  const Type *type = CI->getArgOperand(1)->getType()->getPointerElementType();
  if (const StructType *structType = dyn_cast<StructType>(type)) {
    if (!structType->isOpaque()) {
      const StructInfo *stInfo =
          Ctx->structAnalyzer.getStructInfo(structType, M);
      uint64_t stSize = stInfo->getExpandedSize();
      for (auto obj : nPtsGraph[I][srcIndex]) {
        for (int i = 0; i < stSize; i++) {
          if (nQualiArray[I].at(obj + i) == QualifierState::Uninitialized) {
            insertUninit(I, obj + i, uninitArg);

            if (warningReported(I, obj + i))
              continue;
#ifdef PRINTWARN
            OP << "***********Warning: argument check fails @ argument 1: field"
               << i << " not initialized\n";
            OP << "[trace] In function @" << Callee->getName().str()
               << " Instruction:" << *I << "\n";
#endif
            visit.clear();
            warningTy = eToS[getWType(I->getOperand(1)->getType())];
            printRelatedBB(obj + i, I, visit, warningTy, 1, i);
            warningSet.insert(obj + i);
          }
        }
      }
    }
  } else {
    for (auto obj : nPtsGraph[I][srcIndex]) {
      if (nQualiArray[I].at(obj) == QualifierState::Uninitialized) {
        insertUninit(I, obj, uninitArg);

        if (warningReported(I, obj))
          continue;
#ifdef PRINTWARN
        OP << "***********Warning: argument check fails @ argument 1: data not "
              "initialized\n";
        OP << "[trace] In function @" << Callee->getName().str()
           << " Instruction:" << *I << "\n";
#endif
        visit.clear();
        printRelatedBB(srcIndex, I, visit, "DATA", 1);
        warningSet.insert(srcIndex);
      }
    }
  }
  if (nQualiArray[I].at(sizeIndex) != QualifierState::Initialized) {
    insertUninit(I, sizeIndex, uninitArg);
  }

  if (nQualiArray[I].at(sizeIndex) == QualifierState::Uninitialized) {
    if (!warningReported(I, sizeIndex)) {
#ifdef PRINTWARN
      OP << "***********Warning: argument check fails @ argument 3: size not "
            "initialized\n";
      OP << "[trace] In function @" << Callee->getName().str()
         << " Instruction:" << *I << "\n";
#endif
      visit.clear();
      printRelatedBB(sizeIndex, I, visit, "DATA", 3);
      warningSet.insert(sizeIndex);
    }
  }
}
void FuncAnalysis::checkFuncs(llvm::Instruction *I, llvm::Function *Callee) {
  if (Callee->empty()) {
    auto FIter = Ctx->Funcs.find(Callee->getName().str());
    if (FIter != Ctx->Funcs.end()) {
      Callee = FIter->second;
    }
  }
  if (Ctx->FSummaries.find(Callee) == Ctx->FSummaries.end()) {
    return;
  }
  CallInst *CI = dyn_cast<CallInst>(I);
  Instruction *entry = &(F->front().front());
  NodeIndex entryIndex = nodeFactory.getValueNodeFor(entry);
  std::set<const Instruction *> visit;
  int argNums = CI->arg_size();
  std::vector<const Argument *> calleeArgs;
  std::string warningTy = eToS[OTHER];

  for (Function::arg_iterator itr = Callee->arg_begin(),
                              ite = Callee->arg_end();
       itr != ite; ++itr) {
    const Argument *arg = &*itr;
    calleeArgs.push_back(arg);
  }
  for (int argNo = 0; argNo < argNums; argNo++) {
    // for var arg functions, check the rest of the argument to be init
    if (argNo >= calleeArgs.size()) {
      NodeIndex argNode = nodeFactory.getValueNodeFor(CI->getArgOperand(argNo));
      if (nQualiArray[I].at(argNode) == QualifierState::Uninitialized) {
        insertUninit(I, argNode, uninitArg);
        if (!warningReported(I, argNode)) {
#ifdef PRINTWARN
          OP << "***********Warning: argument check fails @ argument " << argNo
             << " : ptr not initialized, Node " << argNode
             << ", actual qualifier = " << nQualiArray[I].at(argNode) << "\n";
          OP << "[trace] In function @" << F->getName().str()
             << " Instruction:" << *I << "\n";
#endif
          visit.clear();
          printRelatedBB(argNode, I, visit, eToS[NORMAL_PTR], argNo);
          warningSet.insert(argNode);
        }
      }
      for (auto obj : nPtsGraph[I][argNode]) {
        if (obj <= nodeFactory.getConstantIntNode())
          continue;
        unsigned objSize = nodeFactory.getObjectSize(obj);
        unsigned objOffset = nodeFactory.getObjectOffset(obj);
        for (unsigned i = 0; i < objSize; i++) {
          if (warningReported(I, obj - objOffset + i))
            continue;
          if (nQualiArray[I].at(obj - objOffset + i) == QualifierState::Uninitialized) {
            insertUninit(I, obj - objOffset + i, uninitArg);
          }

          if (nQualiArray[I].at(obj - objOffset + i) == QualifierState::Uninitialized) {
#ifdef PRINTWARN
            OP << "***********Warning: argument check fails @ argument "
               << argNo << " : field " << i << " not initialized1";
            OP << ", actual qualifier is "
               << nQualiArray[I].at(obj - objOffset + i)
               << ", nodeIndex = " << obj - objOffset + i << "\n";
            OP << "[trace] In function @" << F->getName().str()
               << " Instruction:" << *I << "\n";
#endif
            visit.clear();
            warningTy =
                eToS[getFieldTy(CI->getArgOperand(argNo)->getType(), i)];
            printRelatedBB(obj - objOffset + i, I, visit, warningTy, argNo, i);
            warningSet.insert(obj - objOffset + i);
          }
        }
      }
      continue;
    }

    NodeIndex argNode = nodeFactory.getValueNodeFor(CI->getArgOperand(argNo));
    NodeIndex sumArgNode =
        Ctx->FSummaries[Callee].args[argNo + 1].getNodeIndex();
    assert(argNode != AndersNodeFactory::InvalidIndex &&
           "Failed to find arg node");

    // else it's the pointer type
    // check weather it's one variable or pointer qualifier
    if (nQualiArray[I].at(argNode) == QualifierState::Uninitialized) {
      insertUninit(I, argNode, uninitArg);
    }

    if (nQualiArray[I].at(argNode) != QualifierState::Unknown &&
        nQualiArray[I].at(argNode) <
            Ctx->FSummaries[Callee].reqVec.at(sumArgNode)) {
      if (!warningReported(I, argNode)) {
#ifdef PRINTWARN
        OP << "***********Warning: argument check fails @ argument " << argNo
           << " : ptr not initialized, Node " << argNode
           << ", actual qualifier = " << nQualiArray[I].at(argNode) << "\n";
        OP << "[trace] In function @" << F->getName().str()
           << " Instruction:" << *I << "\n";
#endif
        visit.clear();
        printRelatedBB(argNode, I, visit, eToS[NORMAL_PTR], argNo);
        warningSet.insert(argNode);
      }
    }
    // if the argument is a pointer, then we check the type and the size of the
    // object.
    unsigned checkSize = 0;
    if (CI->getArgOperand(argNo)->getType()->isPointerTy()) {
      const Type *type =
          CI->getArgOperand(argNo)->getType()->getPointerElementType();
      /// An array is considered a single variable of its type.
      while (const ArrayType *arrayType = dyn_cast<ArrayType>(type))
        type = arrayType->getElementType();
      if (const StructType *structType = dyn_cast<StructType>(type)) {
        if (structType->isOpaque()) {
        } else if (!structType->isLiteral() &&
                   structType->getStructName().startswith("union")) {
          checkSize = 1;
        } else {
          const StructInfo *stInfo =
              Ctx->structAnalyzer.getStructInfo(structType, M);
          checkSize = stInfo->getExpandedSize();
        }
      }
    }
    for (auto sumObj : Ctx->FSummaries[Callee].sumPtsGraph[sumArgNode]) {
      unsigned sumObjSize = Ctx->FSummaries[Callee].args[argNo].getObjSize();
      unsigned sumObjOffset = Ctx->FSummaries[Callee].args[argNo].getOffset();

      for (auto obj : nPtsGraph[I][argNode]) {

        if (obj < nodeFactory.getConstantIntNode())
          continue;

        unsigned objSize = nodeFactory.getObjectSize(obj);
        // 1. check the obj -> end of the obj
        unsigned objOffset = nodeFactory.getObjectOffset(obj);
        checkSize = std::min(checkSize, sumObjSize);
        checkSize = std::min(checkSize, objSize);
        for (unsigned i = 0; i < checkSize; i++) {
          if (nodeFactory.isUnOrArrObjNode(obj)) {
            if (nQualiArray[I].at(obj) == QualifierState::Initialized) {
              insertUninit(I, obj, uninitArg);
            }
            if (nQualiArray[I].at(obj) == QualifierState::Uninitialized) {
              if (!warningReported(I, obj)) {
#ifdef PRINTWARN
                OP << "***********Warning: argument check fails @ argument "
                   << argNo << " some field inside union not initialized2";
                OP << ", actual qualifier is " << nQualiArray[I][obj]
                   << ", nodeIndex = " << obj << "\n";
                OP << "[trace] In function @" << F->getName().str()
                   << " Instruction:" << *I << "\n";
#endif
                visit.clear();
                printRelatedBB(obj, I, visit, "DATA", argNo);
                warningSet.insert(obj);
              }
            }
          }
          if (obj + i >= nodeFactory.getNumNodes())
            continue;
          if (nQualiArray[I].at(obj + i) != QualifierState::Initialized) {
            insertUninit(I, obj + i, uninitArg);
          }

          if (nQualiArray[I].at(obj + i) != QualifierState::Unknown &&
              nQualiArray[I].at(obj + i) <
                  Ctx->FSummaries[Callee].reqVec.at(sumObj + i)) {
            if (!warningReported(I, obj + i)) {
#ifdef PRINTWARN
              OP << "***********Warning: argument check fails @ argument "
                 << argNo << " : field " << i << " not initialized3";
              OP << ", actual qualifier is " << nQualiArray[I].at(obj + i)
                 << ", nodeIndex = " << obj + i << "\n";
              OP << "[trace] In function @" << F->getName().str()
                 << " Instruction:" << *I << "\n";
#endif
              warningTy =
                  eToS[getFieldTy(CI->getArgOperand(argNo)->getType(), i)];
              visit.clear();
              printRelatedBB(obj + i, I, visit, warningTy, argNo, i);
              warningSet.insert(obj + i);
            }
          }
        }
        // 2. if offset != 0
        if (objOffset > 0 && sumObjOffset > 0) {
          for (unsigned i = 0; i < objOffset; i++) {
            if (nQualiArray[I].at(obj - i) == QualifierState::Uninitialized) {
              insertUninit(I, obj - i, uninitArg);
            }

            if (nQualiArray[I].at(obj - i) != QualifierState::Unknown &&
                nQualiArray[I].at(obj - i) <
                    Ctx->FSummaries[Callee].reqVec.at(sumObj - i)) {
              if (warningReported(I, obj - i))
                continue;
#ifdef PRINTWARN
              OP << "***********Warning: argument check fails @ argument "
                 << argNo << " : field " << objOffset - i << " not initialized";
              OP << ", actual qualifier is " << nQualiArray[I].at(obj - i)
                 << ", nodeIndex = " << obj - i << "\n";
              OP << "[trace] In function @" << F->getName().str()
                 << " Instruction:" << *I << "\n";
#endif
              visit.clear();
              warningTy =
                  eToS[getFieldTy(CI->getArgOperand(argNo)->getType(), i)];
              printRelatedBB(obj - i, I, visit, warningTy, argNo,
                             objOffset - i);
              warningSet.insert(obj - i);
            }
          }
        }
      }
    }
  } // for arg iterator
}
void FuncAnalysis::propInitFuncs(llvm::Instruction *I,
                                 RequirednessArray &reqArray) {
  CallInst *CI = dyn_cast<CallInst>(I);
  NodeIndex ptrNode = nodeFactory.getValueNodeFor(CI->getArgOperand(0));
  reqArray[ptrNode] = RequirednessState::Required;
  return;
}
void FuncAnalysis::propCopyFuncs(llvm::Instruction *I,
                                 RequirednessArray &reqArray) {
  CallInst *CI = dyn_cast<CallInst>(I);
  if (CI->arg_size() < 3)
    return;
  // 1. copy the qualifier inference from src to dst
  Value *dst = CI->getArgOperand(0);
  Value *src = CI->getArgOperand(1);
  Value *size = CI->getArgOperand(2);
  NodeIndex dstIndex = nodeFactory.getValueNodeFor(dst);
  NodeIndex srcIndex = nodeFactory.getValueNodeFor(src);
  NodeIndex sizeIndex = nodeFactory.getValueNodeFor(size);

  reqArray[dstIndex] = RequirednessState::Required;
  reqArray[srcIndex] = RequirednessState::Required;
  reqArray[sizeIndex] = RequirednessState::Required;
  if (!dst->getType()->isPointerTy() || !src->getType()->isPointerTy()) {
    return;
  }
  for (auto obj : nPtsGraph[I][srcIndex]) {
    if (obj <= nodeFactory.getConstantIntNode())
      continue;
    unsigned objSize = nodeFactory.getObjectSize(obj);
    unsigned objOffset = nodeFactory.getObjectOffset(obj);
    for (auto dstObj : nPtsGraph[I][dstIndex]) {
      if (dstObj < nodeFactory.getConstantIntNode()) {
        for (unsigned i = 0; i < objSize - objOffset; i++) {
          reqArray[dstObj + i] = RequirednessState::Required;
        }
      } else {
        for (unsigned i = 0; i < objSize - objOffset; i++) {
          reqArray[dstObj + i] = reqArray[obj + i];
        }
      }
    }
  }
}
void FuncAnalysis::propTransferFuncs(llvm::Instruction *I,
                                     RequirednessArray &reqArray) {
  CallInst *CI = dyn_cast<CallInst>(I);
  // 1. copy the qualifier inference from src to dst
  Value *dst = CI->getArgOperand(0);
  Value *src = CI->getArgOperand(1);
  Value *size = CI->getArgOperand(2);
  NodeIndex dstIndex = nodeFactory.getValueNodeFor(dst);
  NodeIndex srcIndex = nodeFactory.getValueNodeFor(src);
  NodeIndex sizeIndex = nodeFactory.getValueNodeFor(size);

  reqArray[dstIndex] = RequirednessState::Required;
  reqArray[srcIndex] = RequirednessState::Required;
  reqArray[sizeIndex] = RequirednessState::Required;

  if (!dst->getType()->isPointerTy() || !src->getType()->isPointerTy()) {
    return;
  }
  for (auto obj : nPtsGraph[I][srcIndex]) {
    if (obj <= nodeFactory.getConstantIntNode())
      continue;
    unsigned objSize = nodeFactory.getObjectSize(obj);
    unsigned objOffset = nodeFactory.getObjectOffset(obj);
    for (unsigned i = 0; i < objSize - objOffset; i++) {
      reqArray[obj + i] = RequirednessState::Required;
    }
  }
}
void FuncAnalysis::propFuncs(llvm::Instruction *I, llvm::Function *Callee,
                             RequirednessArray &reqArray) {
  if (Callee == NULL || Ctx->FSummaries.find(Callee) == Ctx->FSummaries.end())
    return;
  CallInst *CI = dyn_cast<CallInst>(I);
  std::vector<const Argument *> calleeArgs;
  for (auto const &a : Callee->args()) {
    const Argument *arg = &a;
    calleeArgs.push_back(arg);
  }
  for (int argNo = 0; argNo < (int)CI->arg_size(); argNo++) {
    NodeIndex argNode = nodeFactory.getValueNodeFor(CI->getArgOperand(argNo));
    if (argNo >= static_cast<int>(calleeArgs.size()))
      break;
    NodeIndex sumArgNode =
        Ctx->FSummaries[Callee].sumNodeFactory.getValueNodeFor(
            calleeArgs.at(argNo));
    if (sumArgNode == AndersNodeFactory::InvalidIndex)
      continue;
    if (Ctx->FSummaries[Callee].reqVec.at(sumArgNode) ==
        QualifierState::Initialized) {
      reqArray[argNode] = RequirednessState::Required;
    }
    NodeIndex sumArgObjNode = 0;
    auto sumPtsIt = Ctx->FSummaries[Callee].sumPtsGraph.find(sumArgNode);
    if (sumPtsIt == Ctx->FSummaries[Callee].sumPtsGraph.end() ||
        sumPtsIt->second.empty()) {
      continue;
    }
    for (auto sumObj : sumPtsIt->second) {
      sumArgObjNode = sumObj;
    }
    unsigned sumArgObjOffset =
        Ctx->FSummaries[Callee].sumNodeFactory.getObjectOffset(sumArgObjNode);
    unsigned sumArgObjSize =
        Ctx->FSummaries[Callee].sumNodeFactory.getObjectSize(sumArgObjNode);
    for (auto argObj : nPtsGraph[I][argNode]) {
      unsigned objOffset = nodeFactory.getObjectOffset(argObj);
      unsigned objSize = nodeFactory.getObjectSize(argObj);
      for (unsigned i = sumArgObjOffset; i < sumArgObjSize; i++) {
        if (Ctx->FSummaries[Callee].reqVec.at(sumArgObjNode + i) == QualifierState::Initialized)
          reqArray[argObj + i] = RequirednessState::Required;
      }
      if (objOffset != 0) {
        for (unsigned i = 0; i < sumArgObjOffset; i++) {
          if (Ctx->FSummaries[Callee].reqVec.at(sumArgObjNode - i) == QualifierState::Initialized)
            reqArray[argObj - i] = RequirednessState::Required;
        }
      }
    }
  }
}
