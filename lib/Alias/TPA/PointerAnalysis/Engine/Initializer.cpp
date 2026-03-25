// Implementation of the Initializer class.
//
// This class is responsible for bootstrapping the data-flow analysis.
// It sets up the initial analysis state at the entry point of the program
// (usually 'main').
//
// Key Responsibilities:
// 1. Locate the entry function and its CFG.
// 2. Model the command-line arguments (argv) and environment variables (envp)
//    passed to the entry function.
// 3. Initialize the worklist with the entry program point.
// 4. Seed the memoization table with the initial store (containing globals).

#include "Alias/TPA/PointerAnalysis/Engine/Initializer.h"

#include "llvm/Support/raw_ostream.h"

#include "Alias/TPA/Context/Context.h"
#include "Alias/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/PointerManager.h"
#include "Alias/TPA/PointerAnalysis/Program/SemiSparseProgram.h"
#include "Alias/TPA/PointerAnalysis/Support/Memo.h"
#include "Alias/TPA/Util/Log.h"

namespace tpa {

// Runs the initialization phase.
// Takes the initial store (populated with global variable initializers)
// and returns the initial worklist containing the entry point.
ForwardWorkList Initializer::runOnInitState(Store &&initStore) {
  ForwardWorkList workList;

  const auto *entryCtx = context::Context::getGlobalContext();
  const auto *entryCFG = globalState.getSemiSparseProgram().getEntryCFG();
  assert(entryCFG != nullptr);
  const auto *entryNode = entryCFG->getEntryNode();
  if (!entryCFG || !entryNode) {
    LOG_ERROR("TPA initializer error: entry CFG or entry node missing");
    return workList;
  }
  LOG_DEBUG("TPA initializer: entry CFG={} entry node={}",
            static_cast<const void *>(entryCFG),
            static_cast<const void *>(entryNode));

  // Set up argv and envp for the entry function (e.g., main(argc, argv, envp))
  auto &entryFunc = entryCFG->getFunction();
  LOG_DEBUG("TPA initializer: entry function={} args={}",
            entryFunc.getName().str(), entryFunc.arg_size());

  // Handle 'argv' (2nd argument of main: char **argv).
  // argv is a pointer to an array of char* strings. We model it as:
  //   argvPtr  -> { argvObj }          (env: argv variable points to the array)
  //   argvObj  -> { Universal }        (store: each argv[i] may point anywhere,
  //                                     i.e. to any string in memory)
  // Bug fix: previously argvObj was stored as pointing to itself
  // (initStore.insert(argvObj, argvObj)), which meant dereferencing argv[i]
  // would yield the argv array object rather than a string buffer. The correct
  // model is that argv[i] is a char* that can point to any string — represented
  // conservatively as Universal.
  if (entryFunc.arg_size() > 1) {
    const auto *argvValue = std::next(entryFunc.arg_begin());
    const auto *argvPtr =
        globalState.getPointerManager().getOrCreatePointer(entryCtx, argvValue);
    LOG_DEBUG("TPA initializer: argv ptr={}",
              static_cast<const void *>(argvPtr));

    // Allocate a memory object for the argv array.
    const auto *argvObj =
        globalState.getMemoryManager().allocateArgv(argvValue);
    LOG_DEBUG("TPA initializer: argv obj={}",
              static_cast<const void *>(argvObj));

    // argv variable points to the argv array object.
    globalState.getEnv().insert(argvPtr, argvObj);
    // Each element of argv (argv[i]) is a char* that may point to any string.
    // Use weakUpdate(obj, PtsSet) to store a full PtsSet into the store entry.
    initStore.weakUpdate(
        argvObj, PtsSet::getSingletonSet(MemoryManager::getUniversalObject()));

    // Handle 'envp' (3rd argument of main: char **envp).
    // Same model as argv: envp[i] may point to any string.
    if (entryFunc.arg_size() > 2) {
      const auto *envpValue = std::next(argvValue);
      const auto *envpPtr = globalState.getPointerManager().getOrCreatePointer(
          entryCtx, envpValue);
      LOG_DEBUG("TPA initializer: envp ptr={}",
                static_cast<const void *>(envpPtr));

      const auto *envpObj =
          globalState.getMemoryManager().allocateEnvp(envpValue);
      LOG_DEBUG("TPA initializer: envp obj={}",
                static_cast<const void *>(envpObj));

      globalState.getEnv().insert(envpPtr, envpObj);
      initStore.weakUpdate(envpObj, PtsSet::getSingletonSet(
                                        MemoryManager::getUniversalObject()));
    }
  }

  // Create the initial program point at the entry of the function
  auto pp = ProgramPoint(entryCtx, entryNode);
  LOG_DEBUG("TPA initializer: initial program point ready");

  // Seed the memo table with the initial store
  memo.update(pp, std::move(initStore));
  LOG_DEBUG("TPA initializer: memo updated");

  // Add the entry point to the worklist to start analysis
  workList.enqueue(pp);
  LOG_DEBUG("TPA initializer: worklist enqueued");

  return workList;
}

} // namespace tpa
