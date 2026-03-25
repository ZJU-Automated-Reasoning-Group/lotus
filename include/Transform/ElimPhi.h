/**
 * Copyright (C) 2015-2016 Veronika Sokova <xsokov00@stud.fit.vutbr.cz>
 *
 * Eliminate PHINode -> assignment moves to previous BasicBlock,
 * new LLVM IR is in non-SSA form
 */

#ifndef H_ELIM_PHI_H
#define H_ELIM_PHI_H

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"

#include <unordered_map>

using namespace llvm;

struct EliminatePHIPass : public FunctionPass {

  typedef std::unordered_map<PHINode *, Value *> AssignMap;

public:
  static char ID;

  EliminatePHIPass() : FunctionPass(ID) {}
  virtual bool doInitialization(Module &);
  virtual bool runOnFunction(Function &);
  virtual bool doFinalization(Module &);

private:
  AssignMap PHIMap;

  bool testPHI(BasicBlock *);
  Value *getAssign(PHINode *);
  void replacePHIAssign(BasicBlock *fromBB, BasicBlock *phiBB, BranchInst *,
                        unsigned);
};

#endif /* H_ELIM_PHI_H */