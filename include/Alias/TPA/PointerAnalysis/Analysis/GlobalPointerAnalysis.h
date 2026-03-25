#pragma once

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/TypeMap.h"
#include "Alias/TPA/PointerAnalysis/Support/Env.h"
#include "Alias/TPA/PointerAnalysis/Support/Store.h"

namespace context {
class Context;
} // namespace context

namespace llvm {
class Constant;
class ConstantExpr;
class DataLayout;
class GlobalValue;
class GlobalVariable;
class Module;
} // namespace llvm

namespace tpa {

class MemoryManager;
class MemoryObject;
class PointerManager;

// Builds the initial points-to state from module-level entities.
//
// This pass-like component prepares the starting Env/Store before fixpoint
// propagation begins:
// 1) create abstract pointers/objects for globals and functions,
// 2) seed special roots (universal/null),
// 3) evaluate global initializers (including nested aggregates and constant
//    GEP/bitcast forms) into Store updates.
//
// The output pair (Env, Store) is consumed by Initializer and then propagated
// by the semi-sparse engine.
class GlobalPointerAnalysis {
private:
  using EnvStore = std::pair<Env, Store>;

  PointerManager &ptrManager;
  MemoryManager &memManager;
  const TypeMap &typeMap;
  const context::Context *globalCtx;

  // Lookup helper: resolve a GlobalValue to the unique MemoryObject created for
  // it during createGlobalVariables/createFunctions.
  const MemoryObject *getGlobalObject(const llvm::GlobalValue *, const Env &);

  // Phase A: bootstrap roots and top-level entities.
  void initializeSpecialPointerObject(const llvm::Module &, EnvStore &);
  void createGlobalVariables(const llvm::Module &, Env &);
  void createFunctions(const llvm::Module &, Env &);

  // Phase B: consume global initializers and write memory-level facts.
  void initializeGlobalValues(const llvm::Module &, EnvStore &);
  void processGlobalInitializer(const MemoryObject *, const llvm::Constant *,
                                EnvStore &, const llvm::DataLayout &);
  void processGlobalScalarInitializer(const MemoryObject *,
                                      const llvm::Constant *, EnvStore &,
                                      const llvm::DataLayout &);
  void processGlobalStructInitializer(const MemoryObject *,
                                      const llvm::Constant *, EnvStore &,
                                      const llvm::DataLayout &);
  void processGlobalArrayInitializer(const MemoryObject *,
                                     const llvm::Constant *, EnvStore &,
                                     const llvm::DataLayout &);
  std::pair<const llvm::GlobalVariable *, size_t>
  processConstantGEP(const llvm::ConstantExpr *, const llvm::DataLayout &);

public:
  GlobalPointerAnalysis(PointerManager &, MemoryManager &, const TypeMap &);

  // Main entry: compute initial Env/Store for the whole module.
  //
  // Returned Env contains top-level pointer bindings (globals/functions/special
  // pointers). Returned Store contains memory contents induced by global
  // initializers and conservative fallbacks for unknown externals.
  EnvStore runOnModule(const llvm::Module &);
};

} // namespace tpa
