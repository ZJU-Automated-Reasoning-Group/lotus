/**
 * @file ValueMapping.cpp
 * @brief Implementation of ValueMapping for mapping LLVM values to Z3
 * expressions.
 *
 * ValueMapping provides a context-sensitive mapping from LLVM values to Z3 SMT
 * expressions at specific program points within a fragment. It handles the
 * SSA-like encoding where values may be "primed" (after definition) or
 * "unprimed" (before definition) depending on their position relative to the
 * mapping point. Also manages memory state variables that change at
 * memory-modifying instructions.
 *
 * @author rainoftime
 */
#include "Verification/SymAbsAI/Core/ValueMapping.h"

#include "Verification/SymAbsAI/Core/FloatingPointModel.h"
#include "Verification/SymAbsAI/Core/MemoryModel.h"

#include <sstream>

namespace symabs_ai {
/**
 * @brief Index operator to get the Z3 expression for an LLVM value.
 *
 * Returns the Z3 expression representing the value at this mapping point.
 * For pointer values, extracts the pointer value from the memory model.
 * For other values, returns the full representation directly.
 *
 * @param value The LLVM value to look up
 * @return Z3 expression representing the value
 */
z3::expr ValueMapping::operator[](llvm::Value *value) const {
  z3::expr res = this->getFullRepresentation(value);
  if (value->getType()->isPointerTy())
    return FunctionContext_.getMemoryModel().get_ptr_value(res);
  else
    return res;
}

/**
 * @brief Get the full Z3 representation of a value, determining primed/unprimed
 * status.
 *
 * Determines whether a value should be represented as primed (_1) or unprimed
 * (_0) based on:
 * - AtBeginning: always unprimed
 * - AtEnd: always primed
 * - At a point: primed if the value is defined in the fragment and reachable
 * before the point
 * - Otherwise: unprimed
 *
 * Arguments are never primed as they don't change within a function.
 *
 * @param value The LLVM value to represent
 * @return Z3 expression (constant) representing the value with appropriate
 * naming
 */
z3::expr ValueMapping::getFullRepresentation(llvm::Value *value) const {
  bool primed;

  if (AtBeginning_) {
    primed = false;
  } else if (AtEnd_) {
    primed = true;
  } else if (Fragment_.defines(value)) {
    auto *inst = llvm::cast<llvm::Instruction>(value);
    primed = (inst != Point_ && Fragment_.reachable(inst, Point_));
  } else {
    primed = false;
  }

  // form appropriate name for primed/unprimed var
  std::string name = value->getName().str();
  if (!llvm::isa<llvm::Argument>(value)) {
    if (primed)
      name = name + "_1";
    else
      name = name + "_0";
  }

  llvm::Type *type = value->getType();
  return FunctionContext_.getZ3().constant(name.c_str(),
                                           FunctionContext_.sortForType(type));
}

/**
 * @brief Get the Z3 expression representing memory state at this mapping point.
 *
 * Memory state changes at memory-modifying instructions (alloca, store, call).
 * This method counts such instructions up to the current point and creates a
 * uniquely named memory variable. The name format is "mem_<bb>_<count>".
 *
 * @return Z3 expression (constant) representing the memory state variable
 */
z3::expr ValueMapping::memory() const {
  llvm::BasicBlock *bb;
  int mem_ops = 0;

  if (Point_ != nullptr) {
    bb = Point_->getParent();
  } else if (AtEnd_) {
    bb = Fragment_.getEnd();
  } else {
    bb = Fragment_.getStart();
  }

  if (bb != nullptr && (Point_ != nullptr || (AtEnd_ && IncludesEndBody_))) {
    for (auto &inst : *bb) {
      if (&inst == Point_)
        break; // never happens if AtEnd_

      // New memory variable is required after alloca, store or call
      // instruction. Transfer formulas that relate these variables are
      // generated in InstructionSemantics (for loads and stores; in case
      // of calls it's just a fresh variable since we cannot assume
      // anything).
      if (llvm::isa<llvm::StoreInst>(inst) ||
          llvm::isa<llvm::AllocaInst>(inst) ||
          llvm::isa<llvm::CallInst>(inst)) {
        ++mem_ops;
      }
    }
  }

  std::ostringstream name;
  name << "mem_";

  if (bb == Fragment::EXIT)
    name << "EXIT";
  else
    name << bb->getName().str();

  name << "_" << mem_ops;

  auto sort = FunctionContext_.getMemoryModel().sort();
  return FunctionContext_.getZ3().constant(name.str().c_str(), sort);
}

/**
 * @brief Create a ValueMapping at the beginning of a basic block (after PHIs).
 *
 * Finds the first non-PHI instruction in the block and creates a mapping
 * before that instruction. Special case: if bb is EXIT, returns atEnd mapping.
 *
 * @param fctx Function context for the mapping
 * @param frag Fragment containing the basic block
 * @param bb Basic block to create mapping for
 * @return ValueMapping positioned at the start of the block's non-PHI
 * instructions
 */
ValueMapping ValueMapping::atLocation(const FunctionContext &fctx,
                                      const Fragment &frag,
                                      llvm::BasicBlock *bb) {
  if (bb == Fragment::EXIT) {
    assert(frag.getEnd() == Fragment::EXIT);
    return atEnd(fctx, frag);
  }

  llvm::Instruction *point = nullptr;

  // find first non-phi instruction
  for (auto &instr : *bb) {
    if (!llvm::isa<llvm::PHINode>(instr)) {
      point = &instr;
      break;
    }
  }

  assert(point != nullptr);
  return ValueMapping::before(fctx, frag, point);
}

/**
 * @brief Create a ValueMapping at the beginning of a fragment.
 *
 * All values are unprimed at the fragment start, representing their state
 * before any fragment instructions execute.
 *
 * @param fctx Function context for the mapping
 * @param frag Fragment to create mapping for
 * @return ValueMapping at the fragment beginning
 */
ValueMapping ValueMapping::atBeginning(const FunctionContext &fctx,
                                       const Fragment &frag) {
  ValueMapping result(fctx, frag, nullptr);
  result.AtBeginning_ = true;
  return result;
}

/**
 * @brief Create a ValueMapping at the end of a fragment.
 *
 * All values defined in the fragment are primed at the fragment end,
 * representing their state after all fragment instructions execute.
 *
 * @param fctx Function context for the mapping
 * @param frag Fragment to create mapping for
 * @return ValueMapping at the fragment end
 */
ValueMapping ValueMapping::atEnd(const FunctionContext &fctx,
                                 const Fragment &frag) {
  ValueMapping result(fctx, frag, nullptr);
  result.AtEnd_ = true;
  return result;
}

/**
 * @brief Create a ValueMapping immediately before an instruction.
 *
 * Creates a mapping point just before the given instruction. If the instruction
 * is the first non-PHI in the fragment start block, returns atBeginning
 * instead.
 *
 * @param fctx Function context for the mapping
 * @param frag Fragment containing the instruction
 * @param inst Instruction to create mapping before
 * @return ValueMapping positioned before the instruction
 */
ValueMapping ValueMapping::before(const FunctionContext &fctx,
                                  const Fragment &frag,
                                  llvm::Instruction *inst) {
  // is inst the starting instruction of this fragment?
  if (inst->getParent() == frag.getStart()) {
    auto itr = frag.getStart()->begin();
    while (llvm::isa<llvm::PHINode>(*itr))
      ++itr;

    if (inst == &*itr)
      return ValueMapping::atBeginning(fctx, frag);
  }

  return ValueMapping(fctx, frag, inst);
}

/**
 * @brief Create a ValueMapping immediately after an instruction.
 *
 * Creates a mapping point just after the given instruction. Handles special
 * cases for self-looping fragments (Start == End) where the last PHI in the
 * end block maps to atEnd. The instruction must not be a terminator.
 *
 * @param fctx Function context for the mapping
 * @param frag Fragment containing the instruction
 * @param inst Instruction to create mapping after (must not be a terminator)
 * @return ValueMapping positioned after the instruction
 */
ValueMapping ValueMapping::after(const FunctionContext &fctx,
                                 const Fragment &frag,
                                 llvm::Instruction *inst) {
  assert(!inst->isTerminator());

  // Special case: This fragment is a loop and we're asked for a point after
  // the last instruction in it.
  if (inst->getParent() == frag.getEnd() && frag.getStart() == frag.getEnd()) {

    // is `inst' the last phi instruction in its block?
    if (llvm::isa<llvm::PHINode>(inst)) {
      auto itr = inst->getParent()->begin();
      while (&*itr != inst)
        ++itr;

      ++itr;
      if (itr != inst->getParent()->end() && !llvm::isa<llvm::PHINode>(*itr)) {

        return ValueMapping::atEnd(fctx, frag);
      }
    }
  }

  auto itr = inst->getParent()->begin();
  while (&*itr != inst)
    ++itr;

  // use the next instruction (there must be one)
  ++itr;
  return ValueMapping(fctx, frag, &*itr);
}
} // namespace symabs_ai
