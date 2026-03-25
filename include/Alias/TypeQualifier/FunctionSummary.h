/**
 * @file FunctionSummary.h
 * @brief Function summary structures for TypeQualifier analysis
 *
 * This file provides data structures for representing function summaries
 * in the TypeQualifier analysis framework. It includes node factories,
 * argument information, and points-to graph summaries used for
 * interprocedural analysis.
 *
 * @ingroup TypeQualifier
 */

#ifndef UBIANALYSIS_FUNCTIONSUMMARY_H
#define UBIANALYSIS_FUNCTIONSUMMARY_H
#include "Alias/TypeQualifier/Annotation.h"
#include "Alias/TypeQualifier/Common.h"
#include "Alias/TypeQualifier/PtsSet.h"
#include "Alias/TypeQualifier/QualifierTypes.h"

#include <map>
#include <unordered_map>

using namespace llvm;
typedef unsigned NodeIndex;

using BBList = std::set<std::string>;

class SumAndersNode {
public:
  enum AndersNodeType { VALUE_NODE, OBJ_NODE };

private:
  AndersNodeType type;
  const llvm::Value *value;
  const unsigned offset;
  NodeIndex idx;
  SumAndersNode(AndersNodeType t, unsigned i, const llvm::Value *v = NULL,
                const unsigned off = 0)
      : type(t), value(v), offset(off), idx(i) {}

public:
  NodeIndex getIndex() const { return idx; }
  const llvm::Value *getValue() const { return value; }
  unsigned getOffset() const { return offset; }
  friend class SumNodeFactory;
};

class SumNodeFactory {
public:
  static const unsigned InvalidIndex = std::numeric_limits<unsigned int>::max();

private:
  std::vector<SumAndersNode> nodes;
  static const NodeIndex ReturnIndex = 0;
  // valueNodeMap - This map indicates the AndersNode* that a particular Value*
  // corresponds to
  llvm::DenseMap<const llvm::Value *, NodeIndex> valueNodeMap;

  // ObjectNodes - This map contains entries for each memory object in the
  // program: globals, alloca's and mallocs. We are able to represent them as
  // llvm::Value* because we're modeling the heap with the simplest
  // allocation-site approach
  llvm::DenseMap<const llvm::Value *, NodeIndex> objNodeMap;
  static const NodeIndex ReturnPtrIndex = 0;

public:
  SumNodeFactory() {}
  unsigned getNumNodes() const { return nodes.size(); }
  NodeIndex createValueNode(const llvm::Value *val) {
    unsigned nextIdx = nodes.size();
    nodes.push_back(SumAndersNode(SumAndersNode::VALUE_NODE, nextIdx, val));
    if (val != nullptr) {
      assert(!valueNodeMap.count(val) &&
             "Trying to insert two mappings to revValueNodeMap!");
      valueNodeMap[val] = nextIdx;
    }
    return nextIdx;
  }
  NodeIndex createObjectNode(const llvm::Value *val) {
    unsigned nextIdx = nodes.size();
    nodes.push_back(SumAndersNode(SumAndersNode::OBJ_NODE, nextIdx, val));
    if (val != nullptr) {
      if (objNodeMap.count(val))
        return objNodeMap[val];
      objNodeMap[val] = nextIdx;
    }
    return nextIdx;
  }
  NodeIndex getValueNodeFor(const llvm::Value *val) const {
    auto itr = valueNodeMap.find(val);
    if (itr == valueNodeMap.end()) {
      return InvalidIndex;
    } else {
      return itr->second;
    }
  }
  const llvm::Value *getValueForNode(NodeIndex i) const {
    const SumAndersNode &n = nodes.at(i);
    if (n.getValue() != nullptr)
      return n.getValue();
    else
      return nodes.at(i - n.getOffset()).getValue();
  }
  NodeIndex createObjectNode(const NodeIndex base, const unsigned offset) {
    assert(offset != 0);

    unsigned nextIdx = nodes.size();
    assert(nextIdx == base + offset);
    const Value *val = getValueForNode(base);
    nodes.push_back(
        SumAndersNode(SumAndersNode::OBJ_NODE, nextIdx, nullptr, offset));
    if (val != NULL && isa<Instruction>(val)) {
      const Instruction *Ins = dyn_cast<Instruction>(val);
    }
    return nextIdx;
  }

  NodeIndex getObjectNodeFor(const llvm::Value *val) const {
    auto itr = objNodeMap.find(val);
    if (itr == objNodeMap.end())
      return InvalidIndex;
    else
      return itr->second;
  }

  bool isObjectNode(NodeIndex i) const {
    return (nodes.at(i).type == SumAndersNode::OBJ_NODE);
  }
  NodeIndex getOffsetObjectNode(NodeIndex n, int offset) const {
    assert(isObjectNode(n + offset) && ((nodes.at(n).getOffset() + offset) ==
                                        nodes.at(n + offset).getOffset()));
    return n + offset;
  }
  unsigned getObjectSize(NodeIndex i) const {
    assert(isObjectNode(i));
    unsigned offset = nodes.at(i).getOffset();

    while ((i + 1) < nodes.size() &&
           nodes.at(i + 1).type == SumAndersNode::OBJ_NODE &&
           nodes.at(i + 1).getOffset() == (offset + 1)) {
      ++i;
      ++offset;
    }
    return offset + 1; // offset starts from 0
  }
  unsigned getObjectOffset(NodeIndex i) const {
    assert(isObjectNode(i));
    return nodes.at(i).getOffset();
  }
  void dumpNode(NodeIndex idx) const {

    const SumAndersNode &n = nodes.at(idx);

    if (n.type == SumAndersNode::VALUE_NODE)
      errs() << "V ";
    else if (n.type == SumAndersNode::OBJ_NODE)
      errs() << "O ";
    else
      assert(false && "Wrong type number!");
    errs() << "#" << n.idx << "\t";

    // Dump node value info.
    const Value *val = n.getValue();
    if (val == nullptr) {
      NodeIndex offset = n.getOffset();
      if (offset == 0)
        errs() << "retIndex>";
      else {
        NodeIndex baseIdx = n.getIndex() - offset;
        const Value *base = nodes.at(baseIdx).getValue();
        assert(base != nullptr);

        errs() << "field [" << offset << "] of ";

        Type *BaseTy = base->getType();
        // if (BaseTy && VerboseLevel >= 2)
        //  BaseTy->print(errs());

        if (base->hasName())
          errs() << " : " << base->getName();
      }
    } else if (val != NULL && isa<Function>(val))
      errs() << "f> " << val->getName();
    else
      errs() << "v> " << *val;
    errs() << "\n";
    // Dump source loc info if possible.
    // dumpLocation(val);
  }
};
class ArgInfo {
private:
  // argNo: which argNo it belongs to
  NodeIndex argNo;
  // idx: the pointer or the first argument of the arg
  NodeIndex idx;
  // offset: the offset of the obj
  int offset;
  // objSize: the object size of the obj
  int objSize;
  // blackList and whiteList for the node
  BBList blackList;
  BBList whiteList;
  // For context sensitivity: which index decides its qualifier
  std::set<NodeIndex> relatedArg;
  bool mayNull = false;

public:
  void setNodeArgNo(NodeIndex _idx) { argNo = _idx; }
  NodeIndex getNodeArgNo() { return argNo; }
  void setNodeIndex(NodeIndex _idx) { idx = _idx; }
  void setOffset(NodeIndex _offset) { offset = _offset; }
  void setObjSize(int _objSize) { objSize = _objSize; }
  NodeIndex getNodeIndex() { return idx; }
  int getOffset() { return offset; }
  int getObjSize() { return objSize; }
  void setWhiteList(BBList _whiteList) { whiteList = _whiteList; }
  void setBlackList(BBList _blackList) { blackList = _blackList; }
  BBList getWhiteList() { return whiteList; }
  BBList getBlackList() { return blackList; }
  void addToBlackList(std::string bbName) { blackList.insert(bbName); }
  void addToWhiteList(std::string bbName) { whiteList.insert(bbName); }
  void addToRelatedArg(NodeIndex idx) { relatedArg.insert(idx); }
  std::set<NodeIndex> getRelatedArgs() { return relatedArg; }
  void setMayNull(bool _mayNull) { mayNull = _mayNull; }
  bool getMayNull() { return mayNull; }
};

class Summary {
  typedef std::map<unsigned, std::set<NodeIndex>> PtsGraph;
  typedef std::unordered_map<std::string, llvm::GlobalVariable *> GObjMap;

private:
  int retSize;
  int retOffset;
  int stackVar;
  int uninitStackVar;

public:
  PtsGraph sumPtsGraph;
  SumNodeFactory sumNodeFactory;
  std::map<int, ArgInfo> args;

  unsigned noNodes;
  QualifierArray reqVec;
  QualifierArray updateVec;
  QualifierArray changeVec;
  std::string fname;
  std::set<std::string> relatedBC;

  void setRetSize(int _size) { retSize = _size; }
  void setRetOffset(int _offset) { retOffset = _offset; }
  int getRetNodes() const { return 0; }
  int getRetSize() const { return retSize; };
  int getRetOffset() const { return retOffset; }
  void setStackVar(int _var) { stackVar = _var; }
  int getStackVar() const { return stackVar; }
  void setUninitStackVar(int _var) { uninitStackVar = _var; }
  int getUninitStackVar() const { return uninitStackVar; }
  QualifierState requiredState(NodeIndex idx) const { return reqVec.at(idx); }
  void setRequiredState(NodeIndex idx, QualifierState state) {
    reqVec.at(idx) = state;
  }
  QualifierState requiredInputState(NodeIndex idx) const {
    return requiredState(idx);
  }
  QualifierState updatedState(NodeIndex idx) const { return updateVec.at(idx); }
  void setUpdatedState(NodeIndex idx, QualifierState state) {
    updateVec.at(idx) = state;
  }
  QualifierState returnState() const { return updateVec.at(getRetNodes()); }
  QualifierState returnObjectState(NodeIndex idx) const {
    return updatedState(idx);
  }
  Summary() {
    noNodes = 0;
    retSize = 0;
    retOffset = 0;
    stackVar = 0;
    uninitStackVar = 0;
  }
  bool isEmpty() { return !(reqVec.size() > 0 || updateVec.size() > 0); }
  void copySummary(Summary &S1, Summary &S, llvm::Function *F) {
    unsigned numNodes = S.noNodes;
    S1.fname = getScopeName(F);
    ;
#ifdef copyNF
    if (num == 0) {
      // copy the nodeFactory:
      // returnValueNode|argValueNode|retObjNode|argObjNode
      S1.sumNodeFactory.createValueNode(F);
      for (auto const &a : F->args()) {
        const Argument *arg = &a;
        NodeIndex argNode = S1.sumNodeFactory.createValueNode(arg);
      }
      if (!F->getReturnType()->isVoidTy()) {
        for (auto obj : S.sumPtsGraph[S.sumNodeFactory.getValueNodeFor(F)]) {
          NodeIndex retNode = S1.sumNodeFactory.createObjectNode(F);
          unsigned objSize = S.sumNodeFactory.getObjectSize(obj);
          S1.retSize = objSize;
          S1.retOffset = S.sumNodeFactory.getObjectOffset(obj);
          for (unsigned i = 1; i < objSize; i++) {
            S1.sumNodeFactory.createObjectNode(retNode, i);
          }
        }
      }
      int count = 0;
      for (auto const &a : F->args()) {
        const Argument *arg = &a;
        NodeIndex argNode = S.sumNodeFactory.getValueNodeFor(arg);
        S1.args[count].setNodeIndex(argNode);
        for (auto obj : S.sumPtsGraph[argNode]) {
          unsigned objSize = S.sumNodeFactory.getObjectSize(obj);
          unsigned objOffset = S.sumNodeFactory.getObjectOffset(obj);
          S1.args[count].setObjSize(objSize);
          S1.args[count].setOffset(objOffset);
          NodeIndex newObj = S1.sumNodeFactory.createObjectNode(arg);
          for (unsigned i = 1; i < objSize; i++) {
            S1.sumNodeFactory.createObjectNode(newObj, i);
          }
        }
      }
    }
#endif
    S1.setRetSize(S.getRetSize());
    S1.setRetOffset(S.getRetOffset());
    S1.sumPtsGraph = S.sumPtsGraph;
    S1.noNodes = numNodes;
    S1.reqVec = S.reqVec;
    S1.updateVec = S.updateVec;
    S1.changeVec = S.changeVec;
    S1.args = S.args;
    S1.relatedBC = S.relatedBC;
    S1.setStackVar(S.getStackVar());
    S1.setUninitStackVar(S.getUninitStackVar());
  }
  bool equal(Summary &s2) {
    if (noNodes != s2.noNodes)
      return false;
    for (unsigned i = 0; i < noNodes; i++) {
      if (reqVec[i] != s2.reqVec[i] || updateVec[i] != s2.updateVec[i])
        return false;
    }
    return true;
  }
  void dump() {
    errs() << "sumNodeFactory:\n";
    for (unsigned i = 0; i < noNodes; i++) {
      sumNodeFactory.dumpNode(i);
    }
    errs() << "sumPtsGraph: \n";
    for (auto &ptr : sumPtsGraph) {
      errs() << ptr.first << " ->";
      for (auto obj : ptr.second) {
        errs() << obj;
      }
      errs() << "\n";
    }
    errs() << "idx:  ";
    for (unsigned i = 0; i < noNodes; i++) {
      errs() << "  " << i;
    }
    errs() << "\nreq:  ";
    for (unsigned i = 0; i < noNodes; i++) {
      errs() << " " << reqVec[i] << " ";
    }
    errs() << "\nupdate:";
    for (unsigned i = 0; i < noNodes; i++) {
      errs() << " " << updateVec[i] << " ";
    }
    errs() << "\n";
  }
  void summary() {
    errs() << "noNodes: " << noNodes << "\n";
    for (auto &ptr : sumPtsGraph) {
      errs() << ptr.first << " ->";
      for (auto &obj : ptr.second) {
        errs() << obj;
      }
      errs() << "\n";
    }
    errs() << "# of stackVar = " << getStackVar()
           << ", # of uninit: " << getUninitStackVar() << "\n";
    errs() << "retun idx" << getRetNodes() << ", retSize = " << getRetSize()
           << ", getRetOffset()= " << getRetOffset() << "\n";
    errs() << "args:\n";
    for (int i = 0; i < args.size(); i++) {
      errs() << "args[" << i << "]: idx = " << args[i].getNodeIndex() << ", ";
      for (auto &obj : sumPtsGraph[args[i].getNodeIndex()]) {
        errs() << "obj = " << obj << "\n";
        errs() << "objsize = " << args[i].getObjSize()
               << ", offset = " << args[i].getOffset() << "\n";
      }
      errs() << "\n";
    }
    errs() << "idx:  ";
    for (unsigned i = 0; i < noNodes; i++) {
      errs() << "  " << i;
    }
    errs() << "\nreq:  ";
    for (unsigned i = 0; i < noNodes; i++) {
      errs() << " " << reqVec[i] << " ";
    }
    errs() << "\nupdate:";
    for (unsigned i = 0; i < noNodes; i++) {
      errs() << " " << updateVec[i] << " ";
    }
    errs() << "\nmodified:";
    for (unsigned i = 0; i < noNodes; i++) {
      errs() << " " << changeVec[i] << " ";
    }
    errs() << "\nList set for update:\n";
    for (unsigned i = 0; i < noNodes; i++) {
      errs() << "i = " << i << ", argNo = " << args[i].getNodeArgNo() << "\n";
      errs() << "related nodes:\n";
      for (auto &item : args[i].getRelatedArgs()) {
        errs() << item << ", ";
      }
      errs() << "\n";

      if (args[i].getWhiteList().empty() && args[i].getBlackList().empty())
        continue;
      errs() << "arg idx: " << i << "\n";
      errs() << "argNo : " << args[i].getNodeArgNo() << "\n";
      errs() << "whitelist: \n";
      for (auto &item : args[i].getWhiteList()) {
        errs() << item << ":";
      }
      errs() << "\n";
      errs() << "blacklist: \n";
      for (auto &item : args[i].getBlackList()) {
        errs() << item << ":";
      }
      errs() << "\n";
    }
    errs() << "\nRelated bitcode:\n";
    for (auto &item : relatedBC) {
      errs() << "=" << item << "\n";
    }
  }
};
#endif
