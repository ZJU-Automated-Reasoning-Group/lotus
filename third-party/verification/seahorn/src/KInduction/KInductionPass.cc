// K-Induction pass: drives the KInductionEngine and prints a seahorn-style
// result (sat/unsat/unknown).

#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include "seahorn/KInduction.hh"
#include "seahorn/Support/SeaDebug.h"
#include "seahorn/Support/Stats.hh"
#include "seahorn/config.h"

#include <chrono>

using namespace llvm;

namespace seahorn {

static llvm::RegisterPass<KInductionPass>
    XKInduction("kinduction", "K-Induction verification (PathBMC-based)");

llvm::Pass *createKInductionPass(llvm::raw_ostream *out) {
  return new KInductionPass(out);
}

char KInductionPass::ID = 0;

KInductionPass::KInductionPass(llvm::raw_ostream *out)
    : llvm::ModulePass(ID), m_out(out) {}

void KInductionPass::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  // The engine runs on cloned modules using its own internal pass managers.
  AU.setPreservesAll();
}

static cl::opt<unsigned>
    KInductionKMin("kinduction-k-min",
                   cl::desc("K-Induction: minimum k to try"), cl::init(1));

static cl::opt<unsigned>
    KInductionKMax("kinduction-k-max",
                   cl::desc("K-Induction: maximum k (0 = no limit)"),
                   cl::init(0));

static cl::opt<unsigned> KInductionTimeout(
    "kinduction-timeout",
    cl::desc("K-Induction: total timeout in seconds (0 = no timeout)"),
    cl::init(0));

static cl::opt<std::string>
    KInductionEntry("kinduction-entry",
                    cl::desc("K-Induction: entry function name"),
                    cl::init("main"));

static cl::opt<bool>
    KInductionVerbose("kinduction-verbose",
                      cl::desc("K-Induction: enable progress logging "
                               "(equivalent to --log=kinduction)"),
                      cl::init(false));

bool KInductionPass::runOnModule(llvm::Module &M) {
#ifndef HAVE_CLAM
  if (m_out)
    *m_out << "k-induction: PathBMC engine requires CLAM (HAVE_CLAM). Result: "
              "unknown\n";
  return false;
#else
  if (KInductionVerbose)
    seahorn::SeaEnableLog("kinduction");

  KInductionOptions opts;
  opts.KMin = KInductionKMin;
  opts.KMax = KInductionKMax;
  opts.TimeoutSec = KInductionTimeout;
  opts.EntryName = KInductionEntry;

  KInductionEngine eng(opts);
  KInductionResult r = eng.run(M);

  if (r == KInductionResult::BUG) {
    if (m_out)
      *m_out << "sat\n";
    Stats::sset("Result", "FALSE");
  } else if (r == KInductionResult::SAFE) {
    if (m_out)
      *m_out << "unsat\n";
    Stats::sset("Result", "TRUE");
  } else {
    if (m_out)
      *m_out << "unknown\n";
  }
  return false;
#endif
}

} // namespace seahorn
