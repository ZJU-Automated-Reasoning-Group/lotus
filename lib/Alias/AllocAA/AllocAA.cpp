/**
 * @file AllocAA.cpp
 * @brief Implementation of the allocation-site-aware alias analysis.
 *
 * See AllocAA.h for the full design description.  This file contains:
 *
 *  - Constructor: seeds function-name sets from AliasSpecManager, then runs
 *    four collection passes (CG scope, allocations, primitive arrays,
 *    memoryless functions).
 *  - Public query methods: getPrimitiveArrayAccess, areGEPIndicesConstantOrIV,
 *    areIdenticalGEPAccessesInSameLoop, canPointToTheSameObject,
 *    isReadOnly, isMemoryless.
 *  - Private helpers for each collection pass and for the alias sub-checks.
 */

#include "Alias/AllocAA/AllocAA.h"

#include "Alias/Spec/AliasSpecManager.h"

#include <utility>

AllocAA::AllocAA(Module &M,
                 std::function<llvm::ScalarEvolution &(Function &F)> getSCEV,
                 std::function<llvm::LoopInfo &(Function &F)> getLoopInfo,
                 std::function<llvm::CallGraph &(void)> getCallGraph)
    : M{M}, getSCEV{std::move(getSCEV)}, getLoopInfo{std::move(getLoopInfo)},
      getCallGraph{std::move(getCallGraph)}, CGUnderMain{}, allocatorCalls{},
      allocatorFunctionNames{}, readOnlyFunctionNames{},
      memorylessFunctionNames{}, primitiveArrayGlobals{},
      primitiveArrayLocals{} {

  // -------------------------------------------------------------------------
  // Step 1: Seed function-name sets from the shared AliasSpecManager.
  //
  // We create a *local* specMgr here (separate from the member `specManager`)
  // solely to extract name lists.  The member `specManager` is used later in
  // collectAllocations / isPrimitiveArrayPointer for per-Function queries.
  // -------------------------------------------------------------------------
  lotus::alias::AliasSpecManager specMgr;
  specMgr.initialize(M);

  // Populate allocatorFunctionNames with all names the spec considers
  // allocators (malloc, calloc, realloc, operator new, etc.).
  for (const auto &name : specMgr.getAllocatorNames()) {
    allocatorFunctionNames.insert(name);
  }

  // A function is "memoryless" (from the spec's perspective) if it has no
  // mod/ref effects at all — i.e., it neither reads nor writes any memory
  // through its arguments or return value.  Typical examples: math helpers
  // like abs(), min(), max() that operate purely on integer values.
  // "Memoryless" here means: no pointer-producing effects and no mod/ref
  // effects.
  for (const auto &name : specMgr.getNoEffectNames()) {
    auto mr = specMgr.getModRefInfo(name);
    if (mr.modifiedArgs.empty() && mr.referencedArgs.empty() &&
        !mr.modifiesReturn && !mr.referencesReturn) {
      memorylessFunctionNames.insert(name);
    }
  }

  // -------------------------------------------------------------------------
  // Steps 2-5: Run the four collection passes in dependency order.
  //   2. CGUnderMain  — must come first; all later passes use it as a filter.
  //   3. allocatorCalls — depends on CGUnderMain.
  //   4. primitiveArrayValues — depends on allocatorCalls.
  //   5. memorylessFunctions — depends on CGUnderMain.
  // -------------------------------------------------------------------------
  auto &callGraph = this->getCallGraph();
  collectCGUnderFunctionMain(M, callGraph);
  collectAllocations(M, callGraph);
  collectPrimitiveArrayValues(M);
  collectMemorylessFunctions(M);

  return;
}

std::pair<Value *, GetElementPtrInst *>
AllocAA::getPrimitiveArrayAccess(Value *V) {
  // V must be a LoadInst or StoreInst; extract its pointer operand.
  auto *memOp = getMemoryPointerOperand(V);
  if (!memOp)
    return std::make_pair(nullptr, nullptr);

  // -----------------------------------------------------------------------
  // Pattern 1: Direct access — the pointer operand IS a primitive array.
  //   Example IR:  store i32 42, i32* %arr
  //   where %arr is itself in primitiveArrayLocals or primitiveArrayGlobals.
  // -----------------------------------------------------------------------
  auto *directAccessArray = getPrimitiveArray(memOp);
  if (directAccessArray) {
    // directAccessArray->print(errs() << "Found direct access array: "); errs()
    // << "\n";
  }
  if (directAccessArray)
    return std::make_pair(directAccessArray, nullptr);

  auto empty = std::make_pair(nullptr, nullptr);
  if (auto *gep = dyn_cast<GetElementPtrInst>(memOp)) {
    // The pointer operand is a GEP — check whether the GEP's base is a
    // known primitive array (local or global).

    auto *gepMemOp = gep->getPointerOperand();

    // -----------------------------------------------------------------------
    // Pattern 2: GEP of a local (heap-allocated) primitive array.
    //   Example IR:  %ptr = getelementptr i32, i32* %mallocResult, i64 %i
    //                store i32 0, i32* %ptr
    // -----------------------------------------------------------------------
    auto *localArray = getLocalPrimitiveArray(gepMemOp);
    if (localArray) {
      // localArray->print(errs() << "Found GEP access local array: "); errs()
      // << "\n";
    }
    if (localArray)
      return std::make_pair(localArray, gep);

    // -----------------------------------------------------------------------
    // Pattern 3: GEP of a loaded global array pointer.
    //   Example IR:  %base = load i32*, i32** @globalArrayPtr
    //                %ptr  = getelementptr i32, i32* %base, i64 %i
    //                store i32 0, i32* %ptr
    //   Here gepMemOp is the LoadInst; we extract its pointer operand
    //   (@globalArrayPtr) and check whether it is a primitive-array global.
    // -----------------------------------------------------------------------
    auto *loadMemOp = getMemoryPointerOperand(gepMemOp);
    auto *globalArray =
        loadMemOp ? getGlobalValuePrimitiveArray(loadMemOp) : nullptr;
    if (globalArray) {
      // globalArray->print(errs() << "Found GEP access global array: "); errs()
      // << "\n";
    }
    if (globalArray)
      return std::make_pair(globalArray, gep);
  }
  return empty;
}

// Check that all non-constant indices of GEP are those of monotonic induction
// variables.
//
// The key insight: LLVM's ScalarEvolution represents a loop induction variable
// as an "add recurrence" SCEV (scAddRecExpr), i.e., a value of the form
//   {start, +, step}<loop>
// If every non-constant index is an add-recurrence, the GEP sweeps through
// memory in a predictable, stride-based pattern — which is exactly what
// loop-parallelisation passes need to reason about.
//
// TODO: Replace with more strict check that all uses of the GEP adhere to base
// type of pointer.
bool AllocAA::areGEPIndicesConstantOrIV(GetElementPtrInst *gep) {
  Function *gepFunc = gep->getFunction();
  auto &SE = this->getSCEV(*gepFunc);

  for (auto &indexV : gep->indices()) {
    // Constant indices (e.g., struct field selectors) are always fine.
    if (isa<ConstantInt>(indexV))
      continue;

    // For non-constant indices, ask ScalarEvolution for the SCEV expression.
    // scAddRecExpr is the canonical form for loop induction variables:
    //   {start, +, stride}<enclosing-loop>
    // Any other SCEV type (e.g., scUnknown, scMulExpr) means the index is
    // not a simple IV, so we conservatively return false.
    auto *scev = SE.getSCEV(indexV);
    if (scev->getSCEVType() != scAddRecExpr)
      return false;
  }
  return true;
}

bool AllocAA::areIdenticalGEPAccessesInSameLoop(GetElementPtrInst *gep1,
                                                GetElementPtrInst *gep2) {
  // Trivially identical if they are the same instruction.
  if (gep1 == gep2)
    return true;

  // GEPs in different functions can never be "in the same loop".
  if (gep1->getFunction() != gep2->getFunction())
    return false;

  // Both GEPs must reside in basic blocks that belong to the same innermost
  // loop.  If either is outside any loop, or they are in different loops,
  // we cannot conclude they perform the same access pattern.
  auto &LI = this->getLoopInfo(*gep1->getFunction());
  if (LI.getLoopFor(gep1->getParent()) != LI.getLoopFor(gep2->getParent()))
    return false;

  // Check that the base pointer operands refer to the same object.
  // Two cases are handled:
  //   (a) Both GEPs directly use the same Value* as their pointer operand.
  //   (b) Both GEPs use loads from the same pointer (i.e., they both load
  //       from the same global/alloca and then index into the loaded array).
  auto *gepOp1 = gep1->getPointerOperand();
  auto *gepOp2 = gep2->getPointerOperand();
  if (gepOp1 != gepOp2) {
    // Try case (b): both pointer operands must be loads from the same address.
    Value *accessed = nullptr;
    if (auto *load = dyn_cast<LoadInst>(gepOp1)) {
      accessed = load->getPointerOperand();
    } else
      return false; // gepOp1 is not a load — cannot match.
    if (auto *load = dyn_cast<LoadInst>(gepOp2)) {
      if (accessed != load->getPointerOperand())
        return false; // Loads from different addresses.
    } else
      return false; // gepOp2 is not a load — cannot match.
  }

  // Finally, all index operands must be the same Value* (syntactic equality).
  // This is a conservative structural check: it does not perform value
  // numbering, so two GEPs with equivalent but distinct index computations
  // will not be recognised as identical.
  auto indexCount = 0;
  for (auto &indexV1 : gep1->indices()) {
    auto &indexV2 = *(gep2->idx_begin() + indexCount++);
    if (indexV1 != indexV2)
      return false;
  }

  return true;
}

bool AllocAA::isReadOnly(StringRef functionName) {
  return readOnlyFunctionNames.find(functionName.str()) !=
         readOnlyFunctionNames.end();
}

bool AllocAA::isMemoryless(StringRef functionName) {
  return memorylessFunctionNames.find(functionName.str()) !=
         memorylessFunctionNames.end();
}

void AllocAA::collectCGUnderFunctionMain(Module &M, CallGraph &callGraph) {

  // Require a "main" entry point — the analysis is scoped to the live
  // call graph rooted at main.
  auto *main = M.getFunction("main");
  assert(main != nullptr);

  // BFS over the call graph starting from main.
  // We use a separate `reached` set to avoid revisiting functions and to
  // handle recursive call graphs without infinite loops.
  std::queue<Function *> funcToTraverse;
  std::set<Function *> reached;
  funcToTraverse.push(main);
  reached.insert(main);
  while (!funcToTraverse.empty()) {
    auto *func = funcToTraverse.front();
    funcToTraverse.pop();

    auto *funcCGNode = callGraph[func];
    for (auto &callRecord :
         make_range(funcCGNode->begin(), funcCGNode->end())) {
      auto *F = callRecord.second->getFunction();
      // Skip external declarations (no body to analyse) and null entries
      // (which represent indirect calls whose target is unknown).
      if (!F || F->empty())
        continue;

      if (reached.find(F) != reached.end())
        continue;
      reached.insert(F);
      funcToTraverse.push(F);
    }
  }

  CGUnderMain.clear();
  CGUnderMain.insert(reached.begin(), reached.end());
}

void AllocAA::collectAllocations(Module &M, CallGraph &callGraph) {
  std::set<Function *> allocatorFns;

  // Walk all functions in the module and pick out declarations (i.e., external
  // functions with no body) that the spec manager recognises as allocators.
  // We restrict to declarations because allocators like malloc/calloc are
  // typically not defined in the module under analysis.
  for (auto &F : M) {
    if (F.isDeclaration() && specManager.isAllocator(&F)) {
      allocatorFns.insert(&F);
    }
  }

  // Collect every call site within CGUnderMain that targets one of the
  // identified allocator functions.
  collectFunctionCallsTo(callGraph, allocatorFns, this->allocatorCalls);
}

void AllocAA::collectFunctionCallsTo(CallGraph &callGraph,
                                     std::set<Function *> &called,
                                     std::set<CallInst *> &calls) {
  // For each function reachable from main, inspect its outgoing call-graph
  // edges and record any CallInst that targets a function in `called`.
  for (auto *caller : CGUnderMain) {
    auto *funcCGNode = callGraph[caller];
    for (auto &callRecord :
         make_range(funcCGNode->begin(), funcCGNode->end())) {
      auto *F = callRecord.second->getFunction();
      if (called.find(F) == called.end())
        continue;

      // callRecord.first is a WeakTrackingVH (value handle) to the call site.
      // Dereference it to get the actual Value*, then cast to CallInst.
      auto *vO = &*callRecord.first;
      if (vO) {
        auto v = *vO;
        if (auto *call = dyn_cast<CallInst>(v)) {
          calls.insert(call);
        }
      }
    }
  }
}

bool AllocAA::collectUserInstructions(
    Value *V, std::set<Instruction *> &userInstructions) {
  for (auto *user : V->users()) {
    Instruction *I = nullptr;
    if (isa<Instruction>(user)) {
      // Common case: the user is a regular instruction.
      I = (Instruction *)user;
    } else if (isa<BitCastOperator>(user) || isa<ZExtOperator>(user)) {
      // Constant-expression bitcasts and zero-extensions are not Instructions
      // in the LLVM IR sense, but they have exactly one use that IS an
      // Instruction.  We "look through" them to find that instruction.
      // The hasOneUse() guard ensures we don't accidentally merge multiple
      // instruction users into one.
      if (user->hasOneUse()) {
        auto *operUser = *user->user_begin();
        if (isa<Instruction>(operUser)) {
          I = (Instruction *)operUser;
        }
      }
    }

    // If we could not resolve the user to an Instruction (e.g., it is a
    // constant expression with multiple uses, or some other non-instruction
    // user), we conservatively report failure.  The caller will treat this
    // as "the value has an unrecognised use" and disqualify it.
    if (!I)
      return false;
    userInstructions.insert(I);
  }
  return true;
}

void AllocAA::collectPrimitiveArrayValues(Module &M) {

  // -----------------------------------------------------------------------
  // Part A: Identify primitive-array globals.
  //
  // We consider only module-internal globals (no external linkage) that have
  // at least one use.  External-linkage globals may be modified by code
  // outside the module, so we cannot reason about them.
  // -----------------------------------------------------------------------
  for (auto &GV : M.globals()) {
    // Skip globals that are visible outside the module — we cannot track all
    // their uses.
    if (GV.hasExternalLinkage())
      continue;
    if (GV.getNumUses() == 0)
      continue;

    // Collect all user instructions.  If any user is an unrecognised
    // non-instruction (e.g., a multi-use constant expression), bail out.
    std::set<Instruction *> scopedUsers;
    if (!collectUserInstructions(&GV, scopedUsers))
      continue;

    // Only consider globals that are actually used by functions reachable
    // from main.  Globals used only by dead code are irrelevant.
    bool relevantToMain = false;
    for (auto *I : scopedUsers) {
      relevantToMain |= CGUnderMain.find(I->getFunction()) != CGUnderMain.end();
      if (relevantToMain)
        break;
    }
    if (!relevantToMain)
      continue;

    // Test whether the global is a primitive array pointer (i.e., a global
    // that holds a pointer to a contiguous array whose uses are all
    // well-behaved).
    if (isPrimitiveArrayPointer(&GV, scopedUsers))
      primitiveArrayGlobals.insert(&GV);
  }

  // -----------------------------------------------------------------------
  // Part B: Identify primitive-array locals (heap allocations).
  //
  // For each call site of a known allocator, check whether the returned
  // pointer is used only in ways that qualify it as a primitive array.
  // -----------------------------------------------------------------------
  for (auto *call : allocatorCalls) {
    std::set<Instruction *> allUsers;
    if (!collectUserInstructions(call, allUsers))
      continue;
    if (isPrimitiveArray(call, allUsers))
      primitiveArrayLocals.insert(call);
  }
}

bool AllocAA::isPrimitiveArrayPointer(
    Value *V, std::set<Instruction *> &userInstructions) {
  // A "primitive array pointer" is a value (typically a global variable of
  // pointer type, e.g., `int **g`) that is used exclusively to:
  //   (a) Receive a freshly-allocated array (store of a single-use allocator
  //       call result), or
  //   (b) Provide a pointer to an array that is itself used as a primitive
  //       array (load whose result passes isPrimitiveArray).
  //
  // Any other use (e.g., passing the pointer to an unknown function, storing
  // a non-allocator value) disqualifies the value.
  bool isPrimitive = true;
  for (auto *I : userInstructions) {
    if (auto *store = dyn_cast<StoreInst>(I)) {
      // The store must write a freshly-allocated array into this pointer.
      // "Freshly allocated" means the stored value is a CallInst to a known
      // allocator AND that call has exactly one use (this store), so the
      // allocation is not shared with any other pointer.
      if (auto *storedCall = dyn_cast<CallInst>(store->getValueOperand())) {
        auto *callF = storedCall->getCalledFunction();
        // Conservatively return false for indirect calls — we cannot know
        // what an indirect call returns.
        if (!callF) {
          return false;
        }
        if (specManager.isAllocator(callF)) {
          // hasOneUse() ensures the allocation result flows only into this
          // store and is not aliased by any other pointer.
          if (storedCall->hasOneUse())
            continue;
        }
      }
      // Any other store (non-allocator value, or shared allocation) fails.
    }

    if (auto *load = dyn_cast<LoadInst>(I)) {
      // A load of the pointer is acceptable if the loaded value is itself
      // used only as a primitive array (i.e., all uses of the loaded pointer
      // are GEPs that don't escape, or read-only calls).
      std::set<Instruction *> allUsers;
      if (collectUserInstructions(load, allUsers) &&
          isPrimitiveArray(load, allUsers)) {
        continue;
      }
    }

    // Any other use (call, GEP directly on the pointer-to-pointer, etc.)
    // disqualifies the value.
    isPrimitive = false;
    break;
  }

  return isPrimitive;
}

bool AllocAA::isPrimitiveArray(Value *V,
                               std::set<Instruction *> &userInstructions) {
  // A value is a "primitive array" if every use of it is one of:
  //   1. A CastInst whose result is also a primitive array (recursive).
  //      This handles the common pattern of bitcasting a malloc result from
  //      i8* to the actual element type before indexing.
  //   2. A GetElementPtrInst whose result does not escape (doesValueNotEscape).
  //      GEP-based indexing is the expected access pattern for arrays.
  //   3. A CallInst to a known read-only function.
  //      Read-only functions may inspect the array but cannot store a pointer
  //      to it in a location the analysis cannot track.
  //
  // Any other use (store of the pointer, call to an unknown/write function,
  // return of a pointer type, etc.) disqualifies the value.
  auto isPrimitive = true;
  for (auto *I : userInstructions) {
    // Case 1: Cast — recursively check the cast result.
    if (auto *cast = dyn_cast<CastInst>(I)) {
      std::set<Instruction *> castUsers;
      if (collectUserInstructions(cast, castUsers) &&
          isPrimitiveArray(cast, castUsers))
        continue;
    }
    // Case 2: GEP — acceptable if the GEP result does not escape.
    // We seed the `checked` set with the GEP itself to avoid re-visiting it
    // in the recursive escape analysis.
    if (auto *GEPUser = dyn_cast<GetElementPtrInst>(I)) {
      if (doesValueNotEscape({GEPUser}, GEPUser))
        continue;
    }
    // Case 3: Call to a known read-only function.
    if (auto *callUser = dyn_cast<CallInst>(I)) {
      auto *calleeFn = callUser->getCalledFunction();
      if (calleeFn != nullptr) {
        auto fnName = calleeFn->getName();
        if (readOnlyFunctionNames.find(fnName.str()) !=
            readOnlyFunctionNames.end())
          continue;
      }
    }

    // Any other use disqualifies the value.
    isPrimitive = false;
    break;
  }

  return isPrimitive;
}

bool AllocAA::doesValueNotEscape(std::set<Instruction *> checked,
                                 Instruction *I) {
  // Walk every use of I.  If any use is not an Instruction (e.g., a constant
  // expression that appears in a global initialiser), the value has escaped
  // to a context we cannot analyse.
  User *unkUser = nullptr;
  for (auto *user : I->users()) {
    if (!isa<Instruction>(user)) {
      unkUser = user;
      break;
    }
    auto *userI = cast<Instruction>(user);
    // Cycle guard: if we have already visited this instruction on the current
    // path, skip it to avoid infinite recursion.
    if (checked.find(userI) != checked.end())
      continue;
    checked.insert(userI);

    // -----------------------------------------------------------------------
    // Terminator instructions
    // -----------------------------------------------------------------------
    // Branch and switch instructions use the value only as a condition or
    // selector — they do not propagate the pointer anywhere.
    Instruction *userInst;
    if (true && ((userInst = dyn_cast<Instruction>(user)) != nullptr) &&
        userInst->isTerminator()) {
      if (isa<BranchInst>(user) || isa<SwitchInst>(user))
        continue;
      if (isa<ReturnInst>(user)) {
        // A return of an integer value is acceptable: the integer cannot be
        // reinterpreted as a pointer by any downstream use of the original
        // array value, because at no point along the use chain are pointer-
        // typed instructions permitted.
        //
        // NOTE: Technically, a program could treat the returned integer as
        // a pointer, but since at no point along the uses of the original
        // value are pointer based instructions permitted, no intentional
        // pointer value can be returned here.
        auto *returnV = cast<ReturnInst>(user)->getReturnValue();
        if (isa<IntegerType>(returnV->getType()))
          continue;
      }
      // Any other terminator (e.g., unreachable, invoke) is unknown.
      unkUser = user;
      break;
    }

    // -----------------------------------------------------------------------
    // Store instructions
    // -----------------------------------------------------------------------
    // A store is acceptable only if BOTH the stored value AND the storage
    // location are themselves non-escaping.  This prevents the array pointer
    // from being written into a location that could be read by unknown code.
    if (auto *store = dyn_cast<StoreInst>(user)) {
      auto *stored = store->getValueOperand();
      auto storedDoesNotEscape = false;
      // Only integer-typed stored values are considered non-escaping.
      // A pointer-typed stored value would propagate the array address.
      if (isa<IntegerType>(stored->getType())) {
        if (auto *storedI = dyn_cast<Instruction>(stored)) {
          if (doesValueNotEscape(checked, storedI))
            storedDoesNotEscape = true;
        }
        if (isa<ConstantData>(stored))
          storedDoesNotEscape = true;
      }

      // The storage location must also be non-escaping.  The simplest case
      // is that the storage IS the instruction I itself (i.e., we are storing
      // into the array, not storing the array pointer somewhere else).
      auto *storage = store->getPointerOperand();
      auto storageDoesNotEscape = storage == (Value *)I;
      if (!storageDoesNotEscape) {
        if (auto *storageI = dyn_cast<Instruction>(storage)) {
          if (doesValueNotEscape(checked, storageI))
            storageDoesNotEscape = true;
        }
      }

      if (storedDoesNotEscape && storageDoesNotEscape)
        continue;
      unkUser = user;
      break;
    }

    // -----------------------------------------------------------------------
    // Integer-typed instructions
    // -----------------------------------------------------------------------
    // An integer computation derived from the array pointer (e.g., ptrtoint,
    // arithmetic on the address) is acceptable as long as the integer result
    // itself does not escape.
    if (isa<IntegerType>(userI->getType())) {
      if (doesValueNotEscape(checked, userI))
        continue;
    }
    // Any other use (pointer-typed instruction, call, etc.) is unknown.
    unkUser = user;
    break;
  }

  if (unkUser) {
    return false;
  }
  return true;
}

void AllocAA::collectMemorylessFunctions(Module &M) {
  // Scan every function reachable from main.  A function is "memoryless" if
  // its body contains no instruction that could read or write memory:
  //   - No LoadInst  (reads memory)
  //   - No StoreInst (writes memory)
  //   - No CallInst  (may read/write memory through the callee)
  //   - No operand that is a GlobalValue (accessing a global is a memory op)
  //
  // This is a purely intra-procedural, syntactic check.  It is sound in the
  // sense that if a function passes, it truly has no memory effects.  It is
  // not complete: a function that calls another memoryless function will not
  // be recognised here (the TODO below tracks this limitation).
  for (auto *F : this->CGUnderMain) {

    auto isMemoryless = true;
    for (auto &B : *F) {
      for (auto &I : B) {
        // Any load, store, or call immediately disqualifies the function.
        if (isa<LoadInst>(I) || isa<StoreInst>(I) || isa<CallInst>(I)) {
          isMemoryless = false;
        }

        // Any instruction that references a GlobalValue as an operand is
        // also disqualifying — even if the instruction itself is not a
        // memory op (e.g., a GEP of a global, or a comparison against a
        // global address).
        for (auto &op : I.operands()) {
          if (isa<GlobalValue>(op.get())) {
            isMemoryless = false;
            break;
          }
        }

        if (!isMemoryless)
          break;
      }

      if (!isMemoryless)
        break;
    }

    // TODO(angelo): Trigger a recheck of functions using this function
    // in case they are then found to be memoryless.  Currently, if foo()
    // calls bar() and bar() is memoryless, foo() will not be recognised as
    // memoryless because the CallInst to bar() disqualifies it.
    if (isMemoryless) {
      memorylessFunctionNames.insert(F->getName().str());
    }
  }
}

Value *AllocAA::getPrimitiveArray(Value *V) {
  auto *localArray = getLocalPrimitiveArray(V);
  return localArray ? localArray : getGlobalValuePrimitiveArray(V);
}

Value *AllocAA::getLocalPrimitiveArray(Value *V) {
  auto *targetV = V;
  if (auto *cast = dyn_cast<CastInst>(V))
    targetV = cast->getOperand(0);
  if (auto *I = dyn_cast<Instruction>(targetV)) {
    if (primitiveArrayLocals.find(I) != primitiveArrayLocals.end()) {
      return I;
    }
  }
  return nullptr;
}

Value *AllocAA::getGlobalValuePrimitiveArray(Value *V) {
  auto *targetV = V;
  if (auto *cast = dyn_cast<CastInst>(V))
    targetV = cast->getOperand(0);
  if (auto *GV = dyn_cast<GlobalValue>(targetV)) {
    if (primitiveArrayGlobals.find(GV) != primitiveArrayGlobals.end()) {
      return GV;
    }
  }
  return nullptr;
}

Value *AllocAA::getMemoryPointerOperand(Value *V) {
  if (auto *load = dyn_cast<LoadInst>(V)) {
    return load->getPointerOperand();
  }
  if (auto *store = dyn_cast<StoreInst>(V)) {
    return store->getPointerOperand();
  }
  return nullptr;
}

bool AllocAA::canPointToTheSameObject(Value *p1, Value *p2) {
  // Apply each sub-check in turn.  If any sub-check proves no-alias, return
  // false immediately.  If all sub-checks pass (may-alias), return true.

  // Sub-check 1: argument attributes (readonly argument → no-alias with store).
  if (!this->canPointToTheSameObject_ArgumentAttributes(p1, p2)) {
    return false;
  }

  // Sub-check 2: allocation-origin check (distinct globals/allocas → no-alias).
  if (!this->canPointToTheSameObject_Globals(p1, p2)) {
    return false;
  }

  // Neither sub-check fired — conservatively report may-alias.
  return true;
}

Value *AllocAA::getBasePointer(Value *p) {
  assert(p != nullptr);
  // Strip a single GEP layer to reach the base allocation.
  // We do not recurse through multiple GEPs; a single level is sufficient
  // for the allocation-origin checks performed by
  // canPointToTheSameObject_Globals.
  if (auto *gep = dyn_cast<GetElementPtrInst>(p)) {
    return gep->getPointerOperand();
  }
  return p;
}

bool AllocAA::canPointToTheSameObject_Globals(Value *p1, Value *p2) {
  // Strip GEPs to reach the base allocation of each pointer.
  auto *b1 = this->getBasePointer(p1);
  auto *b2 = this->getBasePointer(p2);

  // Rule 1: A global variable and a stack allocation live in different
  // storage classes and can never alias.
  if (isa<GlobalValue>(b1) && isa<AllocaInst>(b2)) {
    return false;
  }
  if (isa<GlobalValue>(b2) && isa<AllocaInst>(b1)) {
    return false;
  }

  // Rule 2: Two distinct global variables are different objects.
  // (Same global → may-alias, handled by the fall-through.)
  if (isa<GlobalValue>(b1) && isa<GlobalValue>(b2) && (b1 != b2)) {
    return false;
  }

  // Rule 3: Two distinct alloca instructions allocate different stack slots
  // in the same function invocation and can never alias.
  if (isa<AllocaInst>(b1) && isa<AllocaInst>(b2) && (b1 != b2)) {
    return false;
  }

  return true;
}

bool AllocAA::canPointToTheSameObject_ArgumentAttributes(Value *p1, Value *p2) {
  // This check applies only when one operand is a LoadInst and the other is
  // a StoreInst.  In all other combinations (load+load, store+store, or
  // neither), we cannot draw a conclusion and return true (may-alias).

  // Try to identify which of p1/p2 is the load and which is the store.
  auto *loadInst = dyn_cast<LoadInst>(p1);
  if (!loadInst) {
    loadInst = dyn_cast<LoadInst>(p2);
  }
  // If neither is a load, the check does not apply.
  if (loadInst == nullptr) {
    return true;
  }

  auto *storeInst = dyn_cast<StoreInst>(p1);
  if (!storeInst) {
    storeInst = dyn_cast<StoreInst>(p2);
  }
  // If neither is a store, the check does not apply.
  if (storeInst == nullptr) {
    return true;
  }

  // Determine the base pointer of the load: strip a GEP if present so we
  // can inspect the underlying argument.
  auto *loadPtr = loadInst->getPointerOperand();
  if (auto *gep = dyn_cast<GetElementPtrInst>(loadPtr)) {
    loadPtr = gep->getPointerOperand();
  }
  assert(loadPtr != nullptr);

  // The check fires only if the load's base pointer is a function argument
  // marked `readonly` (or `readnone`).  Such an argument guarantees the
  // callee does not write through it, so a load through it cannot alias any
  // store in the same function.
  auto *obj1 = dyn_cast<Argument>(loadPtr);
  if (obj1 == nullptr) {
    return true; // Base is not an argument — check does not apply.
  }
  if (obj1->onlyReadsMemory()) {
    return false; // readonly argument → load cannot alias the store.
  }

  return true;
}
