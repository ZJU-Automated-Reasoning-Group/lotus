//
// This file is distributed under the MIT License. See LICENSE for details.
//

#ifndef SLICING_H
#define SLICING_H

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/InstVisitor.h"
#include <string>
#include <unordered_set>
#include <vector>

using namespace llvm;

namespace smack {

using std::string;
using std::unordered_set;
using std::vector;

class Naming;
class SmackRep;
class Expr;
class Decl;

class Slice;
typedef vector<Slice *> Slices;

class Slice {
  Value &value;
  BasicBlock &block;
  Function &function;
  LLVMContext &context;
  Slices &slices;
  string name;

  unordered_set<Value *> inputs;
  unordered_set<Value *> values;

public:
  Slice(Instruction &I, Slices &S, string name = "");

  void remove();

  string getName();
  const Expr *getCode(Naming *naming, SmackRep *rep);
  const Decl *getBoogieDecl(Naming *naming, SmackRep *rep);
  const Expr *getBoogieExpression(Naming *naming, SmackRep *rep);
};
} // namespace smack

#endif
