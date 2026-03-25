//
// Created by prophe cheng on 2024/1/3.
//

#ifndef TYPEDIVE_COMMON_H
#define TYPEDIVE_COMMON_H

#include <bitset>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <llvm/ADT/Triple.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#include <unistd.h>

using namespace llvm;
using namespace std;

const string vtblLabelBeforeDemangle = "_ZTV";
const string znLabel = "_ZN";

#define OP errs()
#define DBG                                                                    \
  if (debug_mode)                                                              \
  OP

string getInstructionText(Value *inst);

string getInstructionText(Type *type);

string getValueInfo(Value *value);

string getTypeInfo(Type *type);

class Helper {
public:
  // LLVM value
  static string getValueName(Value *v) {
    if (!v->hasName()) {
      return to_string(reinterpret_cast<uintptr_t>(v));
    } else {
      return v->getName().str();
    }
  }

  static string getValueType(Value *v) {
    if (Instruction *inst = dyn_cast<Instruction>(v)) {
      return string(inst->getOpcodeName());
    } else {
      return string("value " + to_string(v->getValueID()));
    }
  }

  static string getValueRepr(Value *v) {
    string str;
    raw_string_ostream stm(str);

    v->print(stm);
    stm.flush();

    return str;
  }
};

// 常用类型定义
using ModuleList = vector<
    pair<Module *,
         StringRef>>; // 模块列表类型，每个模块对应一个Module*对象以及一个模块名
// Mapping module to its file name.
using ModuleNameMap =
    unordered_map<Module *, StringRef>; // 将模块对象映射为模块名的类型
// The set of all functions.
using FuncSet = SmallPtrSet<Function *, 8>;     // 函数集合类型
using CallInstSet = SmallPtrSet<CallInst *, 8>; // Call指令集合类型
using CallerMap =
    DenseMap<Function *, CallInstSet>; // 将Function对象映射为对应的callsite集合
using CalleeMap =
    DenseMap<CallInst *, FuncSet>; // 将Call指令映射为对应的函数集合

class CommonUtil {
public:
  //
  // Common functions
  //
  string getValidStructName(string structName);

  string getValidStructName(StructType *Ty);

  string getFileName(DILocation *Loc, DISubprogram *SP = NULL);

  StringRef getCalledFuncName(CallInst *CI);

  // 获取指令I的源代码位置信息
  DILocation *getSourceLocation(Instruction *I);

  // 获取函数F的第ArgNo个参数对象
  Argument *getParamByArgNo(Function *F, int8_t ArgNo);

  // 根据函数F的FunctionType (返回类型、参数类型、是否支持可变参数)计算F的hash值
  size_t funcHash(Function *F, bool withName = false);

  // 根据callsite对应的FunctionType计算hash
  size_t callHash(CallInst *CI);

  size_t typeHash(Type *Ty);

  size_t typeIdxHash(Type *Ty, int Idx = -1);

  size_t hashIdxHash(size_t Hs, int Idx = -1);

  size_t strIntHash(string str, int i);

  string structTyStr(StructType *STy);

  bool trimPathSlash(string &path, int slash);

  int64_t getGEPOffset(const Value *V, const DataLayout *DL);

  // 从所有模块加载结构体信息，初始化使用
  void LoadElementsStructNameMap(vector<pair<Module *, StringRef>> &Modules);
};

// 保存中间及最终结果的结构体
struct GlobalContext {
  GlobalContext() {}

  // Statistics
  unsigned NumVirtualCall = 0;
  unsigned NumFunctions = 0;
  unsigned NumFirstLayerTypeCalls = 0;
  unsigned NumSecondLayerTypeCalls = 0;
  unsigned NumSecondLayerTargets = 0;
  unsigned NumValidIndirectCalls = 0;
  unsigned NumIndirectCallTargets = 0;
  unsigned NumFirstLayerTargets = 0;
  unsigned NumConfinedFuncs = 0;
  unsigned NumSimpleIndCalls = 0;

  // 全局变量，将变量的hash值映射为变量对象，只保存有initializer的全局变量
  DenseMap<size_t, GlobalVariable *> Globals;

  // 将一个global function的id(uint64_t) 映射到实际Function对象.
  map<uint64_t, Function *> GlobalFuncMap;

  // address-taken函数集合
  FuncSet AddressTakenFuncs;

  // 将一个indirect-callsite映射到target function集合，Map a callsite to all
  // potential callee functions.
  CalleeMap Callees;

  // 将一个function映射到对应的caller集合.
  CallerMap Callers;

  // 将一个函数签名映射为对应函数集合s
  DenseMap<size_t, FuncSet> sigFuncsMap;

  // Indirect call instructions.
  vector<CallInst *> IndirectCallInsts;

  // Modules.
  ModuleList Modules;
  ModuleNameMap ModuleMaps;
  set<string> InvolvedModules;

  CommonUtil util;
};

#endif // TYPEDIVE_COMMON_H
