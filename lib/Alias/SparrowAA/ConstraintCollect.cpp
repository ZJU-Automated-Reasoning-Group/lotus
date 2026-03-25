/**
 * @file ConstraintCollect.cpp
 * @brief Constraint collection phase of Andersen's pointer analysis.
 *
 * This file implements the constraint collection phase that scans the program
 * and generates pointer constraints. It processes global variables, functions,
 * and instructions to build the constraint set that will be solved in later
 * phases.
 *
 * FIXME: The analysis does not use on-the-fly callgraph construction, but uses
 * a lightweight address-taken analysis to get the callee list. See the
 * implementation of Andersen::addConstraintForCall for details.
 *
 * @author rainoftime
 */
#include "Alias/SparrowAA/Andersen.h"
#include "Alias/SparrowAA/Log.h"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/Support/raw_ostream.h>

#define DEBUG_TYPE "andersen"

using namespace llvm;

STATISTIC(NumGlobalVariables, "Number of global variables");
STATISTIC(NumGlobalObjects, "Number of global objects created");
STATISTIC(NumAddrTakenFunctions, "Number of address-taken functions");
STATISTIC(NumReturnNodes, "Number of return nodes created");
STATISTIC(NumVarargNodes, "Number of vararg nodes created");
STATISTIC(NumAllocaNodes, "Number of alloca nodes created");
STATISTIC(NumObjectNodes, "Number of object nodes created");
STATISTIC(NumDirectCalls, "Number of direct function calls");
STATISTIC(NumIndirectCalls, "Number of indirect function calls");
STATISTIC(NumExternalLibCalls, "Number of external library calls");
STATISTIC(NumUnresolvedLibCalls, "Number of unresolved library calls");
STATISTIC(NumCallSites, "Number of call sites processed");
STATISTIC(NumFunctions, "Number of functions analyzed");
STATISTIC(NumPointerInstructions, "Number of pointer instructions processed");

/**
 * @brief Collect constraints from the entire module.
 *
 * This stage scans the program, adding a constraint to the constraints list
 * for each instruction that induces a constraint, and setting up the initial
 * points-to graph. Initializes universal and null pointer constraints, then
 * processes globals and all functions.
 *
 * @param M The module to collect constraints from
 */
void Andersen::collectConstraints(const Module &M) {
  LOG_INFO("collectConstraints: Starting, nodeFactory has {} nodes",
           nodeFactory.getNumNodes());

  // First, the universal ptr points to universal obj, and the universal obj
  // points to itself
  constraints.emplace_back(AndersConstraint::ADDR_OF,
                           nodeFactory.getUniversalPtrNode(),
                           nodeFactory.getUniversalObjNode());
  constraints.emplace_back(AndersConstraint::STORE,
                           nodeFactory.getUniversalObjNode(),
                           nodeFactory.getUniversalObjNode());

  // Next, the null pointer points to the null object.
  constraints.emplace_back(AndersConstraint::ADDR_OF,
                           nodeFactory.getNullPtrNode(),
                           nodeFactory.getNullObjectNode());

  LOG_INFO("collectConstraints: Added {} initial constraints",
           constraints.size());

  // Next, add any constraints on global variables. Associate the address of the
  // global object as pointing to the memory for the global: &G = <G memory>
  // Use initialCtx for globals to ensure consistency with function analysis
  collectConstraintsForGlobals(M, initialCtx);

  LOG_INFO(
      "collectConstraints: After globals, have {} constraints and {} nodes",
      constraints.size(), nodeFactory.getNumNodes());

  // Process every defined function with the initial context; calls will spawn
  // more contexts as needed.
  for (auto const &f : M) {
    if (f.isDeclaration() || f.isIntrinsic())
      continue;
    collectConstraintsForFunction(&f, initialCtx);
  }
}

/**
 * @brief Collect constraints for a single function in a given context.
 *
 * Creates function-scoped nodes (return, vararg, arguments) and processes
 * all instructions in the function to generate constraints. Uses a visited
 * set to avoid reprocessing the same function-context pair.
 *
 * @param f The function to process
 * @param ctx The context key for this function instance
 */
void Andersen::collectConstraintsForFunction(const Function *f,
                                             AndersNodeFactory::CtxKey ctx) {
  FunctionContextKey key{f, ctx};
  if (!visitedFunctions.insert(key).second)
    return;

  ++NumFunctions;

  // Per-context function-scoped nodes
  if (f->getFunctionType()->getReturnType()->isPointerTy()) {
    nodeFactory.createReturnNode(f, ctx);
    ++NumReturnNodes;
  }
  if (f->getFunctionType()->isVarArg()) {
    nodeFactory.createVarargNode(f, ctx);
    ++NumVarargNodes;
  }
  for (Function::const_arg_iterator itr = f->arg_begin(), ite = f->arg_end();
       itr != ite; ++itr) {
    if (isa<PointerType>(itr->getType()))
      nodeFactory.createValueNode(&*itr, ctx);
  }

  // First, create a value node for each instruction with pointer type. It is
  // necessary to do the job here rather than on-the-fly because an instruction
  // may refer to the value node defined before it (e.g. phi nodes)
  for (const_inst_iterator itr = inst_begin(*f), ite = inst_end(*f); itr != ite;
       ++itr) {
    const auto *inst = &*itr.getInstructionIterator();
    if (inst->getType()->isPointerTy()) {
      nodeFactory.createValueNode(inst, ctx);
      ++NumPointerInstructions;
    }
  }

  // Now, collect constraint for each relevant instruction
  for (const_inst_iterator itr = inst_begin(*f), ite = inst_end(*f); itr != ite;
       ++itr) {
    const auto *inst = &*itr.getInstructionIterator();
    collectConstraintsForInstruction(inst, ctx);
  }
}

/**
 * @brief Collect constraints for global variables and address-taken functions.
 *
 * Creates value and object nodes for each global variable and generates
 * ADDR_OF constraints. Also handles address-taken functions by creating
 * function pointer nodes.
 *
 * @param M The module containing the globals
 * @param ctx The context key for global processing
 */
void Andersen::collectConstraintsForGlobals(const Module &M,
                                            AndersNodeFactory::CtxKey ctx) {
  // Create a pointer and an object for each global variable
  for (auto const &globalVal : M.globals()) {
    NodeIndex gVal = nodeFactory.createValueNode(&globalVal, ctx);
    NodeIndex gObj = nodeFactory.createObjectNode(&globalVal, ctx);
    constraints.emplace_back(AndersConstraint::ADDR_OF, gVal, gObj);
    ++NumGlobalVariables;
    ++NumGlobalObjects;
  }

  // Functions and function pointers are also considered global.
  // B7 Fix: collectConstraintsForGlobals previously pre-created return nodes,
  // vararg nodes, and argument nodes for all non-declaration functions using
  // initialCtx.  collectConstraintsForFunction() does the same for each
  // function in its own context.  For context-insensitive analysis (k=0) the
  // contexts are identical so createReturnNode/createValueNode are idempotent
  // and the duplication is harmless.  For k>0, however, the pre-created nodes
  // in initialCtx are never used for functions called in a different context,
  // wasting memory and polluting valueNodeBuckets.
  //
  // Fix: only create the address-taken function pointer/object nodes here
  // (which are genuinely global and context-independent).  The per-function
  // return/vararg/argument nodes are created on demand by
  // collectConstraintsForFunction() in the correct context.
  for (auto const &f : M) {
    // If f is an addr-taken function, create a pointer and an object for it.
    // These are global (context-independent) because the function's address
    // is a compile-time constant.
    if (f.hasAddressTaken()) {
      NodeIndex fVal = nodeFactory.createValueNode(&f, ctx);
      NodeIndex fObj = nodeFactory.createObjectNode(&f, ctx);
      constraints.emplace_back(AndersConstraint::ADDR_OF, fVal, fObj);
      ++NumAddrTakenFunctions;
    }
  }

  // Init globals here since an initializer may refer to a global var/func below
  // it
  for (auto const &globalVal : M.globals()) {
    NodeIndex gObj = nodeFactory.getObjectNodeFor(&globalVal, ctx);
    assert(gObj != AndersNodeFactory::InvalidIndex &&
           "Cannot find global object!");

    if (globalVal.hasDefinitiveInitializer()) {
      addGlobalInitializerConstraints(gObj, globalVal.getInitializer(), ctx);
    } else {
      // If it doesn't have an initializer (i.e. it's defined in another
      // translation unit), it points to the universal set.
      constraints.emplace_back(AndersConstraint::COPY, gObj,
                               nodeFactory.getUniversalObjNode());
    }
  }
}

void Andersen::addGlobalInitializerConstraints(NodeIndex objNode,
                                               const Constant *c,
                                               AndersNodeFactory::CtxKey ctx) {
  if (c->getType()->isSingleValueType()) {
    if (isa<PointerType>(c->getType())) {
      NodeIndex rhsNode = nodeFactory.getObjectNodeForConstant(c, ctx);
      assert(rhsNode != AndersNodeFactory::InvalidIndex &&
             "rhs node not found");
      constraints.emplace_back(AndersConstraint::ADDR_OF, objNode, rhsNode);
    }
  } else if (isa<UndefValue>(c)) {
    // Undefined values carry no pointer information; skip.
  } else if (isa<ConstantAggregateZero>(c)) {
    // B8 Fix: ConstantAggregateZero represents a zero-initialized aggregate
    // (struct, array, or vector).  The previous code fell through to the
    // isNullValue() branch only for scalar null pointers; for aggregates,
    // isNullValue() is also true but the correct treatment is to recurse
    // into each element so that pointer-typed fields are connected to the
    // null object node individually.  For non-pointer element types the
    // recursive call is a no-op (isSingleValueType() && !isPointerTy()).
    for (unsigned i = 0, e = c->getNumOperands(); i != e; ++i)
      addGlobalInitializerConstraints(objNode, cast<Constant>(c->getOperand(i)),
                                      ctx);
  } else if (c->isNullValue()) {
    // Scalar null pointer.
    constraints.emplace_back(AndersConstraint::COPY, objNode,
                             nodeFactory.getNullObjectNode());
  } else {
    // Since we are doing field-insensitive analysis, all objects in the
    // array/struct/vector are pointed-to by the 1st-field pointer.
    // ConstantVector (SIMD vector of constants) is also handled here.
    assert(isa<ConstantArray>(c) || isa<ConstantDataSequential>(c) ||
           isa<ConstantStruct>(c) || isa<ConstantVector>(c));

    for (unsigned i = 0, e = c->getNumOperands(); i != e; ++i)
      addGlobalInitializerConstraints(objNode, cast<Constant>(c->getOperand(i)),
                                      ctx);
  }
}

/**
 * @brief Collect constraints for a single instruction.
 *
 * Dispatches to instruction-specific constraint collectors based on opcode.
 * Handles Alloca, Load, Store, GEP, BitCast, PHI, Select, and Call
 * instructions.
 *
 * @param inst The instruction to process
 * @param ctx The context key for this instruction
 */
void Andersen::collectConstraintsForInstruction(const Instruction *inst,
                                                AndersNodeFactory::CtxKey ctx) {
  switch (inst->getOpcode()) {
  case Instruction::Alloca: {
    NodeIndex valNode = nodeFactory.getValueNodeFor(inst, ctx);
    assert(valNode != AndersNodeFactory::InvalidIndex &&
           "Failed to find alloca value node");
    NodeIndex objNode = nodeFactory.createObjectNode(inst, ctx);
    constraints.emplace_back(AndersConstraint::ADDR_OF, valNode, objNode);
    ++NumAllocaNodes;
    ++NumObjectNodes;
    break;
  }
  case Instruction::Call:
  case Instruction::Invoke: {
    if (const CallBase *cs = dyn_cast<CallBase>(inst)) {
      addConstraintForCall(cs, ctx);
      ++NumCallSites;
    }
    break;
  }
  case Instruction::Ret: {
    if (inst->getNumOperands() > 0 &&
        inst->getOperand(0)->getType()->isPointerTy()) {
      NodeIndex retIndex =
          nodeFactory.getReturnNodeFor(inst->getParent()->getParent(), ctx);
      assert(retIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find return node");
      NodeIndex valIndex =
          nodeFactory.getValueNodeFor(inst->getOperand(0), ctx);
      assert(valIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find return value node");
      constraints.emplace_back(AndersConstraint::COPY, retIndex, valIndex);
    }
    break;
  }
  case Instruction::Load: {
    if (inst->getType()->isPointerTy()) {
      NodeIndex opIndex = nodeFactory.getValueNodeFor(inst->getOperand(0), ctx);
      assert(opIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find load operand node");
      NodeIndex valIndex = nodeFactory.getValueNodeFor(inst, ctx);
      assert(valIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find load value node");
      constraints.emplace_back(AndersConstraint::LOAD, valIndex, opIndex);
    }
    break;
  }
  case Instruction::Store: {
    if (inst->getOperand(0)->getType()->isPointerTy()) {
      NodeIndex srcIndex =
          nodeFactory.getValueNodeFor(inst->getOperand(0), ctx);
      assert(srcIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find store src node");
      NodeIndex dstIndex =
          nodeFactory.getValueNodeFor(inst->getOperand(1), ctx);
      assert(dstIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find store dst node");
      constraints.emplace_back(AndersConstraint::STORE, dstIndex, srcIndex);
    }
    break;
  }
  case Instruction::GetElementPtr: {
    assert(inst->getType()->isPointerTy());

    // P1 = getelementptr P2, ... --> <Copy/P1/P2>
    NodeIndex srcIndex = nodeFactory.getValueNodeFor(inst->getOperand(0), ctx);
    assert(srcIndex != AndersNodeFactory::InvalidIndex &&
           "Failed to find gep src node");
    NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst, ctx);
    assert(dstIndex != AndersNodeFactory::InvalidIndex &&
           "Failed to find gep dst node");

    constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);

    break;
  }
  case Instruction::PHI: {
    if (inst->getType()->isPointerTy()) {
      const PHINode *phiInst = cast<PHINode>(inst);
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(phiInst, ctx);
      assert(dstIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find phi dst node");
      for (unsigned i = 0, e = phiInst->getNumIncomingValues(); i != e; ++i) {
        NodeIndex srcIndex =
            nodeFactory.getValueNodeFor(phiInst->getIncomingValue(i), ctx);
        assert(srcIndex != AndersNodeFactory::InvalidIndex &&
               "Failed to find phi src node");
        constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
      }
    }
    break;
  }
  case Instruction::BitCast: {
    if (inst->getType()->isPointerTy()) {
      NodeIndex srcIndex =
          nodeFactory.getValueNodeFor(inst->getOperand(0), ctx);
      assert(srcIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find bitcast src node");
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst, ctx);
      assert(dstIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find bitcast dst node");
      constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
    }
    break;
  }
  case Instruction::IntToPtr: {
    assert(inst->getType()->isPointerTy());

    // Get the node index for dst
    NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst, ctx);
    assert(dstIndex != AndersNodeFactory::InvalidIndex &&
           "Failed to find inttoptr dst node");

    // We use pattern matching to look for a matching ptrtoint
    Value *op = inst->getOperand(0);

    // Pointer copy: Y = inttoptr (ptrtoint X)
    Value *srcValue = nullptr;
    if (PatternMatch::match(
            op, PatternMatch::m_PtrToInt(PatternMatch::m_Value(srcValue)))) {
      NodeIndex srcIndex = nodeFactory.getValueNodeFor(srcValue, ctx);
      assert(srcIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find inttoptr src node");
      constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
      break;
    }

    // Pointer arithmetic: Y = inttoptr (ptrtoint (X) + offset)
    if (PatternMatch::match(
            op, PatternMatch::m_Add(
                    PatternMatch::m_PtrToInt(PatternMatch::m_Value(srcValue)),
                    PatternMatch::m_Value()))) {
      NodeIndex srcIndex = nodeFactory.getValueNodeFor(srcValue, ctx);
      assert(srcIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find inttoptr src node");
      constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
      break;
    }

    // Otherwise, we really don't know what dst points to
    constraints.emplace_back(AndersConstraint::COPY, dstIndex,
                             nodeFactory.getUniversalPtrNode());

    break;
  }
  case Instruction::Select: {
    if (inst->getType()->isPointerTy()) {
      NodeIndex srcIndex1 =
          nodeFactory.getValueNodeFor(inst->getOperand(1), ctx);
      assert(srcIndex1 != AndersNodeFactory::InvalidIndex &&
             "Failed to find select src node 1");
      NodeIndex srcIndex2 =
          nodeFactory.getValueNodeFor(inst->getOperand(2), ctx);
      assert(srcIndex2 != AndersNodeFactory::InvalidIndex &&
             "Failed to find select src node 2");
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst, ctx);
      assert(dstIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find select dst node");
      constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex1);
      constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex2);
    }
    break;
  }
  case Instruction::VAArg: {
    if (inst->getType()->isPointerTy()) {
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst, ctx);
      assert(dstIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find va_arg dst node");
      NodeIndex vaIndex =
          nodeFactory.getVarargNodeFor(inst->getParent()->getParent(), ctx);
      assert(vaIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find vararg node");
      constraints.emplace_back(AndersConstraint::COPY, dstIndex, vaIndex);
    }
    break;
  }
  case Instruction::ExtractValue: {
    // ExtractValue extracts a value from an aggregate (struct/array)
    if (inst->getType()->isPointerTy()) {
      // We're extracting a pointer from a struct/array
      // Conservative approach: the extracted pointer could point to anything
      // that any pointer in the aggregate could point to
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(inst, ctx);
      assert(dstIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find extractvalue dst node");

      // Check if the aggregate operand is also tracked (it might be if it
      // contains pointers)
      Value *aggOperand = inst->getOperand(0);
      NodeIndex srcIndex = nodeFactory.getValueNodeFor(aggOperand, ctx);
      if (srcIndex != AndersNodeFactory::InvalidIndex) {
        // Conservative: the extracted pointer inherits the points-to set of the
        // aggregate
        constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
      } else {
        // The aggregate is not tracked (no pointers involved in its creation)
        // Most conservative: could point to anything
        constraints.emplace_back(AndersConstraint::COPY, dstIndex,
                                 nodeFactory.getUniversalPtrNode());
      }
    }
    break;
  }
  case Instruction::InsertValue: {
    // InsertValue inserts a value into an aggregate (struct/array).
    // The result type is always an aggregate, never a raw pointer, so the
    // old guard `inst->getType()->isPointerTy()` was always false and the
    // entire case was dead code.  We now check whether the inserted value
    // is a pointer and, if so, track it through a synthetic value node for
    // the aggregate result so that subsequent ExtractValue instructions can
    // recover the points-to information.
    Value *insertedVal = inst->getOperand(1);
    if (insertedVal->getType()->isPointerTy()) {
      // Create / retrieve a value node for the aggregate result so that a
      // later ExtractValue can copy from it.
      NodeIndex dstIndex = nodeFactory.createValueNode(inst, ctx);

      // 1. Propagate points-to info from the original aggregate (if tracked).
      Value *aggOperand = inst->getOperand(0);
      NodeIndex srcIndex = nodeFactory.getValueNodeFor(aggOperand, ctx);
      if (srcIndex != AndersNodeFactory::InvalidIndex) {
        constraints.emplace_back(AndersConstraint::COPY, dstIndex, srcIndex);
      }

      // 2. Propagate points-to info from the inserted pointer value.
      NodeIndex insertedIndex = nodeFactory.getValueNodeFor(insertedVal, ctx);
      assert(insertedIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find insertvalue inserted value node");
      constraints.emplace_back(AndersConstraint::COPY, dstIndex, insertedIndex);
    }
    break;
  }
  // We have no intention to support exception-handling in the near future. Just
  // ignore EH-related instructions instead of crashing.
  case Instruction::LandingPad:
  case Instruction::Resume: {
    break;
  }
  // Atomic instructions can be modeled conservatively by their non-atomic
  // counterparts.
  case Instruction::AtomicRMW: {
    const auto *ar = cast<AtomicRMWInst>(inst);

    // Load-like effect: the result is the old value in memory.
    if (ar->getType()->isPointerTy()) {
      NodeIndex ptrIndex =
          nodeFactory.getValueNodeFor(ar->getPointerOperand(), ctx);
      if (ptrIndex != AndersNodeFactory::InvalidIndex) {
        NodeIndex resIndex = nodeFactory.getValueNodeFor(ar, ctx);
        assert(resIndex != AndersNodeFactory::InvalidIndex &&
               "Failed to find atomicrmw result node");
        constraints.emplace_back(AndersConstraint::LOAD, resIndex, ptrIndex);
      }
    }

    // Store-like effect: the new value is written back to memory.
    const Value *valOp = ar->getValOperand();
    if (valOp->getType()->isPointerTy()) {
      NodeIndex srcIndex = nodeFactory.getValueNodeFor(valOp, ctx);
      if (srcIndex != AndersNodeFactory::InvalidIndex) {
        NodeIndex dstIndex =
            nodeFactory.getValueNodeFor(ar->getPointerOperand(), ctx);
        assert(dstIndex != AndersNodeFactory::InvalidIndex &&
               "Failed to find atomicrmw pointer node");
        constraints.emplace_back(AndersConstraint::STORE, dstIndex, srcIndex);
      }
    }
    break;
  }
  case Instruction::AtomicCmpXchg: {
    const auto *cx = cast<AtomicCmpXchgInst>(inst);

    // Store-like effect: if the exchanged-in value is a pointer, it may be
    // written to memory.
    const Value *newVal = cx->getNewValOperand();
    if (newVal->getType()->isPointerTy()) {
      NodeIndex srcIndex = nodeFactory.getValueNodeFor(newVal, ctx);
      if (srcIndex != AndersNodeFactory::InvalidIndex) {
        NodeIndex dstIndex =
            nodeFactory.getValueNodeFor(cx->getPointerOperand(), ctx);
        assert(dstIndex != AndersNodeFactory::InvalidIndex &&
               "Failed to find cmpxchg pointer node");
        constraints.emplace_back(AndersConstraint::STORE, dstIndex, srcIndex);
      }
    }

    // B2 Fix: model the load-side of cmpxchg.
    // The result is a struct {T, i1}.  When T is a pointer type, the first
    // element (the old value read from memory) carries pointer information
    // that must be tracked.  We model this with a LOAD constraint from the
    // pointer operand into a synthetic value node for the instruction result.
    // Subsequent ExtractValue instructions that pull out the old pointer will
    // then copy from this synthetic node via the InsertValue/ExtractValue
    // handling above.
    //
    // Note: the instruction result type is always a struct (never a raw
    // pointer), so we check the *compared* value's type to determine whether
    // the loaded element is a pointer.
    const Value *cmpVal = cx->getCompareOperand();
    if (cmpVal->getType()->isPointerTy()) {
      NodeIndex ptrIndex =
          nodeFactory.getValueNodeFor(cx->getPointerOperand(), ctx);
      if (ptrIndex != AndersNodeFactory::InvalidIndex) {
        // Create a value node for the aggregate result so that a later
        // ExtractValue can copy the old pointer out of it.
        NodeIndex resIndex = nodeFactory.createValueNode(inst, ctx);
        constraints.emplace_back(AndersConstraint::LOAD, resIndex, ptrIndex);
      }
    }
    break;
  }
  default: {
    if (inst->getType()->isPointerTy()) {
      LOG_ERROR("pointer-related inst not handled: {}", *inst);
      llvm_unreachable("pointer-related inst not handled!");
    }
    break;
  }
  }
}

// There are two types of constraints to add for a function call:
// - ValueNode(callsite) = ReturnNode(call target)
// - ValueNode(formal arg) = ValueNode(actual arg)
/**
 * @brief Add constraints for a function call instruction.
 *
 * Handles both direct and indirect calls. For direct calls, processes the
 * called function. For indirect calls, uses address-taken analysis to find
 * potential callees. Spawns new contexts for context-sensitive analysis.
 *
 * @param cs The call site instruction
 * @param callerCtx The context of the calling function
 */
void Andersen::addConstraintForCall(const llvm::CallBase *cs,
                                    AndersNodeFactory::CtxKey callerCtx) {
  if (const Function *f = cs->getCalledFunction()) // Direct call
  {
    ++NumDirectCalls;
    if (f->isDeclaration() || f->isIntrinsic()) // External library call
    {
      ++NumExternalLibCalls;
      // Handle libraries separately
      if (addConstraintForExternalLibrary(cs, f, callerCtx)) {
        return;
      } else { // Unresolved library call: ruin everything!
        ++NumUnresolvedLibCalls;
        // errs() << "Unresolved ext function: " << f->getName() << "\n";
        if (cs->getType()->isPointerTy()) {
          NodeIndex retIndex = nodeFactory.getValueNodeFor(cs, callerCtx);
          assert(retIndex != AndersNodeFactory::InvalidIndex &&
                 "Failed to find ret node!");
          constraints.emplace_back(AndersConstraint::COPY, retIndex,
                                   nodeFactory.getUniversalPtrNode());
        }
        for (unsigned i = 0; i < cs->arg_size(); ++i) {
          Value *argVal = cs->getArgOperand(i);
          if (argVal->getType()->isPointerTy()) {
            NodeIndex argIndex = nodeFactory.getValueNodeFor(argVal, callerCtx);
            assert(argIndex != AndersNodeFactory::InvalidIndex &&
                   "Failed to find arg node!");
            constraints.emplace_back(AndersConstraint::COPY, argIndex,
                                     nodeFactory.getUniversalPtrNode());
          }
        }
      }
    } else // Non-external function call
    {
      AndersNodeFactory::CtxKey calleeCtx = ctxPolicy.evolve(callerCtx, cs);
      collectConstraintsForFunction(f, calleeCtx);

      if (cs->getType()->isPointerTy()) {
        NodeIndex retIndex = nodeFactory.getValueNodeFor(cs, callerCtx);
        assert(retIndex != AndersNodeFactory::InvalidIndex &&
               "Failed to find ret node!");
        NodeIndex fRetIndex = nodeFactory.getReturnNodeFor(f, calleeCtx);
        assert(fRetIndex != AndersNodeFactory::InvalidIndex &&
               "Failed to find function ret node!");
        constraints.emplace_back(AndersConstraint::COPY, retIndex, fRetIndex);
      }
      // The argument constraints
      addArgumentConstraintForCall(cs, f, callerCtx, calleeCtx);
    }
  } else // Indirect call
  {
    ++NumIndirectCalls;
    // For argument constraints, first search through all addr-taken functions:
    // any function that takes can take as many variables is a potential
    // candidate.
    // NOTE: We use f.hasAddressTaken() (the correct LLVM predicate) rather
    // than checking whether a value node exists for f in the current context,
    // which was previously wrong and could miss or spuriously include callees.
    const Module *M = cs->getFunction()->getParent();
    // B6 Fix: helper to check whether a candidate function's type is
    // compatible with the call-site's function-pointer type.  We require
    // that the return types and all argument types are pointer-compatible
    // (both pointer or both non-pointer) to avoid spurious constraints from
    // completely unrelated functions that happen to have the same arity.
    // This is a conservative approximation: it may still admit some
    // incompatible functions, but it eliminates the most egregious mismatches
    // (e.g., a void*(int*) call site matching a void(int,int) callee).
    auto isTypeCompatible = [&](const Function &f) -> bool {
      // Derive the function type from the call-site's called operand.
      // In LLVM 14 opaque-pointer mode, CallBase::getFunctionType() gives the
      // statically-known callee type directly without needing
      // getPointerElementType().
      FunctionType *callSiteFTy = cs->getFunctionType();
      if (!callSiteFTy)
        return true; // Cannot determine type; be conservative.

      FunctionType *calleeFTy = f.getFunctionType();

      // Return type compatibility: both pointer or both non-pointer.
      bool csRetIsPtr = callSiteFTy->getReturnType()->isPointerTy();
      bool fRetIsPtr = calleeFTy->getReturnType()->isPointerTy();
      if (csRetIsPtr != fRetIsPtr)
        return false;

      // Argument type compatibility (for fixed args).
      unsigned numFixed =
          std::min(callSiteFTy->getNumParams(), calleeFTy->getNumParams());
      for (unsigned i = 0; i < numFixed; ++i) {
        bool csArgIsPtr = callSiteFTy->getParamType(i)->isPointerTy();
        bool fArgIsPtr = calleeFTy->getParamType(i)->isPointerTy();
        if (csArgIsPtr != fArgIsPtr)
          return false;
      }
      return true;
    };

    bool foundAnyCallee = false;
    for (auto const &f : *M) {
      // Only consider address-taken functions as indirect-call targets.
      if (!f.hasAddressTaken())
        continue;

      if (!f.getFunctionType()->isVarArg() && f.arg_size() != cs->arg_size())
        // #arg mismatch
        continue;

      // B6 Fix: skip functions whose type is incompatible with the call site.
      if (!isTypeCompatible(f))
        continue;

      foundAnyCallee = true;

      if (f.isDeclaration() || f.isIntrinsic()) // External library call
      {
        if (addConstraintForExternalLibrary(cs, &f, callerCtx))
          continue;
        else {
          // Pollute everything
          for (unsigned i = 0; i < cs->arg_size(); ++i) {
            Value *argVal = cs->getArgOperand(i);
            if (argVal->getType()->isPointerTy()) {
              NodeIndex argIndex =
                  nodeFactory.getValueNodeFor(argVal, callerCtx);
              assert(argIndex != AndersNodeFactory::InvalidIndex &&
                     "Failed to find arg node!");
              constraints.emplace_back(AndersConstraint::COPY, argIndex,
                                       nodeFactory.getUniversalPtrNode());
            }
          }
          // An unresolved external callee means the return is also unknown.
          if (cs->getType()->isPointerTy()) {
            NodeIndex retIndex = nodeFactory.getValueNodeFor(cs, callerCtx);
            assert(retIndex != AndersNodeFactory::InvalidIndex &&
                   "Failed to find ret node!");
            constraints.emplace_back(AndersConstraint::COPY, retIndex,
                                     nodeFactory.getUniversalPtrNode());
          }
        }
      } else {
        AndersNodeFactory::CtxKey calleeCtx = ctxPolicy.evolve(callerCtx, cs);
        collectConstraintsForFunction(&f, calleeCtx);

        // Connect the callee's return node to the call-site value node.
        if (cs->getType()->isPointerTy()) {
          NodeIndex retIndex = nodeFactory.getValueNodeFor(cs, callerCtx);
          assert(retIndex != AndersNodeFactory::InvalidIndex &&
                 "Failed to find ret node!");
          NodeIndex fRetIndex = nodeFactory.getReturnNodeFor(&f, calleeCtx);
          if (fRetIndex != AndersNodeFactory::InvalidIndex)
            constraints.emplace_back(AndersConstraint::COPY, retIndex,
                                     fRetIndex);
        }

        addArgumentConstraintForCall(cs, &f, callerCtx, calleeCtx);
      }
    }

    // If no callee was found at all (the function pointer has no address-taken
    // candidates with a matching arity), fall back conservatively: the return
    // value and pointer arguments may be anything.  Previously this universal
    // constraint was added unconditionally, which made every indirect call
    // return MayAlias with everything even when precise callee return nodes
    // had already been connected above.
    if (!foundAnyCallee) {
      if (cs->getType()->isPointerTy()) {
        NodeIndex retIndex = nodeFactory.getValueNodeFor(cs, callerCtx);
        assert(retIndex != AndersNodeFactory::InvalidIndex &&
               "Failed to find ret node!");
        constraints.emplace_back(AndersConstraint::COPY, retIndex,
                                 nodeFactory.getUniversalPtrNode());
      }
      for (unsigned i = 0; i < cs->arg_size(); ++i) {
        Value *argVal = cs->getArgOperand(i);
        if (argVal->getType()->isPointerTy()) {
          NodeIndex argIndex = nodeFactory.getValueNodeFor(argVal, callerCtx);
          if (argIndex != AndersNodeFactory::InvalidIndex)
            constraints.emplace_back(AndersConstraint::COPY, argIndex,
                                     nodeFactory.getUniversalPtrNode());
        }
      }
    }
  }
}

void Andersen::addArgumentConstraintForCall(
    const llvm::CallBase *cs, const Function *f,
    AndersNodeFactory::CtxKey callerCtx, AndersNodeFactory::CtxKey calleeCtx) {
  Function::const_arg_iterator fItr = f->arg_begin();
  unsigned argIdx = 0;

  while (fItr != f->arg_end() && argIdx < cs->arg_size()) {
    const Argument *formal = &*fItr;
    const Value *actual = cs->getArgOperand(argIdx);

    if (formal->getType()->isPointerTy()) {
      NodeIndex fIndex = nodeFactory.getValueNodeFor(formal, calleeCtx);
      assert(fIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find formal arg node!");
      if (actual->getType()->isPointerTy()) {
        NodeIndex aIndex = nodeFactory.getValueNodeFor(actual, callerCtx);
        assert(aIndex != AndersNodeFactory::InvalidIndex &&
               "Failed to find actual arg node!");
        constraints.emplace_back(AndersConstraint::COPY, fIndex, aIndex);
      } else
        constraints.emplace_back(AndersConstraint::COPY, fIndex,
                                 nodeFactory.getUniversalPtrNode());
    }

    ++fItr;
    ++argIdx;
  }

  // Copy all pointers passed through the varargs section to the varargs node
  if (f->getFunctionType()->isVarArg()) {
    while (argIdx < cs->arg_size()) {
      const Value *actual = cs->getArgOperand(argIdx);
      if (actual->getType()->isPointerTy()) {
        NodeIndex aIndex = nodeFactory.getValueNodeFor(actual, callerCtx);
        assert(aIndex != AndersNodeFactory::InvalidIndex &&
               "Failed to find actual arg node!");
        NodeIndex vaIndex = nodeFactory.getVarargNodeFor(f, calleeCtx);
        assert(vaIndex != AndersNodeFactory::InvalidIndex &&
               "Failed to find vararg node!");
        constraints.emplace_back(AndersConstraint::COPY, vaIndex, aIndex);
      }

      ++argIdx;
    }
  }
}

// The implementation of addConstraintForExternalLibrary is in
// ExternalLibrary.cpp so we remove the duplicate implementation here
