//===- SSIfy.cpp - Transform programs to other program representations ----===//
//
//    This file is licensed under the General Public License v2.
//
//===----------------------------------------------------------------------===//

// Author: rainoftime

#include "IR/SSI/SSI.h"

using namespace llvm;

// Definition of static members of SSIfy
const std::string SSIfy::phiname = "SSIfy_phi";
const std::string SSIfy::signame = "SSIfy_sigma";
const std::string SSIfy::copname = "SSIfy_copy";

cl::opt<bool> Verbose("v", cl::desc("Print details"));

// Fix for bug #10: make -set optional with a sensible default so the pass can
// be used programmatically without requiring a command-line flag.
static cl::opt<std::string>
    ProgramPointOptions("set",
                        cl::desc("Starting program points (4-char string, "
                                 "each char 0 or 1: cond-down, cond-up, "
                                 "uses-down, uses-up)"),
                        cl::init("1100"));

bool SSIfy::runOnFunction(Function &F) {
  this->F = &F;
  this->DTw = &getAnalysis<DominatorTreeWrapperPass>();
  this->DTmap = &(this->DTw->getDomTree());
  this->PDTmap = &getAnalysis<PostDominatorTreeWrapperPass>().getPostDomTree();
  this->DFmap =
      &getAnalysis<DominanceFrontierWrapperPass>().getDominanceFrontier();
  this->PDFmap = new PostDominanceFrontier(this->PDTmap);

  // Fix for bug #11: validate the length of the -set string before indexing.
  const std::string flags_str = std::string(ProgramPointOptions.c_str());
  for (int i = 0; i < 4; ++i) {
    this->flags[i] = (i < (int)flags_str.size()) && (flags_str[i] == '1');
  }

  // Clear per-function side-table sets (fix for bug #2).
  this->ssiPhiSet.clear();
  this->ssiSigmaSet.clear();
  this->ssiCopySet.clear();

  if (Verbose) {
    errs() << "Running on function " << F.getName() << "\n";
  }

  // For every instruction in this function, call the SSIfy function.
  for (Function::iterator Fit = F.begin(), Fend = F.end(); Fit != Fend; ++Fit) {
    BasicBlock &BB = *Fit;
    for (BasicBlock::iterator BBit = BB.begin(), BBend = BB.end();
         BBit != BBend; ++BBit) {
      Instruction &I = *BBit;
      run(&I);
    }
  }

  clean();

  delete this->PDFmap;
  this->PDFmap = nullptr;

  this->versions.clear();
  this->ssiPhiSet.clear();
  this->ssiSigmaSet.clear();
  this->ssiCopySet.clear();

  return true;
}

void SSIfy::run(Instruction *V) {
  std::set<ProgramPoint> Iup;
  std::set<ProgramPoint> Idown;

  // %condition = icmp i32 slt %V 0
  // br i1 %condition BB1 BB2
  // This example above explains this code section below.
  // We have to check if a use of a use of V is a branch instruction to assess
  // whether it is a program point of Out(Conds) or not.
  for (Value::user_iterator i = V->user_begin(), e = V->user_end(); i != e;
       ++i) {
    User *U = *i;
    Instruction *use_inst = dyn_cast<Instruction>(U);
    if (!use_inst)
      continue;

    // Out(Conds)
    if (CmpInst *possible_cmp = dyn_cast<CmpInst>(use_inst)) {
      for (Value::user_iterator ii = possible_cmp->user_begin(),
                                ee = possible_cmp->user_end();
           ii != ee; ++ii) {
        User *Uu = *ii;
        if (BranchInst *br_inst = dyn_cast<BranchInst>(Uu)) {
          // (downwards)
          if (flags[0]) {
            Idown.insert(ProgramPoint(br_inst, ProgramPoint::Out));
          }

          // (upwards)
          if (flags[1]) {
            Iup.insert(ProgramPoint(br_inst, ProgramPoint::Out));
          }
        }
      }
    }
    // Uses
    //
    // EXCEPTIONS
    //  - Terminator instructions
    //  - PHINode
    //
    // These are exceptions because a copy created for them would
    // break the program, or not make sense.
    //
    else if (V->getType()->isIntegerTy()) {
      if (!use_inst->isTerminator() && !isa<PHINode>(use_inst)) {
        // Uses (downwards)
        if (flags[2]) {
          Idown.insert(ProgramPoint(use_inst, ProgramPoint::Self));
        }

        // Uses (upwards)
        if (flags[3]) {
          Iup.insert(ProgramPoint(use_inst, ProgramPoint::Self));
        }
      }
    }
  }

  split(V, Iup, Idown);
  rename_initial(V);
}

void SSIfy::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<PostDominatorTreeWrapperPass>();
  AU.addRequired<DominanceFrontierWrapperPass>();
}

char SSIfy::ID = 0;
static RegisterPass<SSIfy> X("ssify", "SSIfy pass");
