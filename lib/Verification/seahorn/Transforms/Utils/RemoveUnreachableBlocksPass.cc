#include "seahorn/Transforms/Utils/RemoveUnreachableBlocksPass.hh"
#include "llvm/Transforms/Utils/Local.h"

#include "seahorn/config.h"

#include "Alias/seadsa/DsaAnalysis.hh"
#include "Alias/seadsa/ShadowMem.hh"

using namespace llvm;

namespace seahorn
{
  char RemoveUnreachableBlocksPass::ID = 0;

  bool RemoveUnreachableBlocksPass::runOnFunction (Function &F)
  {return removeUnreachableBlocks (F);}

  void RemoveUnreachableBlocksPass::getAnalysisUsage (AnalysisUsage &AU) const {
      // Preserve Sea-DSA passes
      AU.addPreservedID(seadsa::DsaAnalysis::ID);
      AU.addPreservedID(seadsa::ShadowMemPass::ID);      
  }
} // namespace seahorn

namespace seahorn {
    Pass* createRemoveUnreachableBlocksPass()
    {return new RemoveUnreachableBlocksPass();}
} // namespace seahorn
