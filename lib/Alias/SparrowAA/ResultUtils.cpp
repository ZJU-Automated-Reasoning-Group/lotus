/**
 * @file ResultUtils.cpp
 * @brief Result utilities for Sparrow-AA.
 *
 * This file provides utility functions for printing and querying Andersen's
 * pointer analysis results, including points-to sets and alias relationships.
 * Useful for debugging and result inspection.
 *
 * @author rainoftime
 */
#include "Alias/SparrowAA/ResultUtils.h"

#include "Alias/SparrowAA/Andersen.h"
#include "Alias/SparrowAA/AndersenAA.h"

#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace sparrow_aa {

/**
 * @brief Print a value's name or representation to an output stream.
 *
 * @param V The value to print
 * @param OS The output stream
 */
static void printValue(const Value *V, raw_ostream &OS) {
  if (V->hasName())
    OS << V->getName();
  else
    V->printAsOperand(OS, false);
}

/**
 * @brief Print the points-to set for a value.
 *
 * Retrieves and prints all objects that the given pointer value may point to,
 * including annotations for global variables, stack allocations, heap
 * allocations, and function pointers.
 *
 * @param V The pointer value to query
 * @param Anders The Andersen analysis instance
 * @param OS The output stream
 */
void printPointsToSet(const Value *V, Andersen &Anders, raw_ostream &OS) {
  if (!V->getType()->isPointerTy())
    return;

  std::vector<const Value *> ptsSet;
  if (!Anders.getPointsToSet(V, ptsSet)) {
    OS << "  ";
    printValue(V, OS);
    OS << " points to unknown\n";
    return;
  }
  if (ptsSet.empty()) {
    OS << "  ";
    printValue(V, OS);
    OS << " points to nothing\n";
    return;
  }

  OS << "  ";
  printValue(V, OS);
  OS << " points to " << ptsSet.size() << " location(s):\n";
  for (const Value *T : ptsSet) {
    OS << "    - ";
    printValue(T, OS);
    if (isa<GlobalVariable>(T))
      OS << " [global]";
    else if (isa<AllocaInst>(T))
      OS << " [stack]";
    else if (isa<CallInst>(T) || isa<InvokeInst>(T))
      OS << " [heap]";
    else if (isa<Function>(T))
      OS << " [function]";
    OS << "\n";
  }
}

/**
 * @brief Perform alias queries for all pointer pairs in a module.
 *
 * Queries the alias relationship between all pairs of pointers (up to a limit)
 * and prints the results along with a summary of alias statistics.
 *
 * @param M The module to analyze
 * @param AAResult The alias analysis result
 * @param OS The output stream
 */
void performAliasQueries(Module &M, AndersenAAResult &AAResult,
                         raw_ostream &OS) {
  OS << "\n=== Alias Query Results ===\n\n";

  std::vector<const Value *> Pointers;
  for (const GlobalVariable &GV : M.globals())
    if (GV.getType()->isPointerTy())
      Pointers.push_back(&GV);
  for (const Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (const BasicBlock &BB : F)
      for (const Instruction &I : BB)
        if (I.getType()->isPointerTy())
          Pointers.push_back(&I);
  }

  OS << "Total pointers: " << Pointers.size() << "\n\n";

  unsigned Counts[3] = {0};
  const char *Names[] = {"NoAlias", "MayAlias", "MustAlias"};

  for (size_t i = 0; i < Pointers.size() && i < 20; ++i) {
    for (size_t j = i + 1; j < Pointers.size() && j < 20; ++j) {
      AliasResult R = AAResult.alias(
          MemoryLocation(Pointers[i], LocationSize::beforeOrAfterPointer()),
          MemoryLocation(Pointers[j], LocationSize::beforeOrAfterPointer()));
      int idx = (R == AliasResult::MayAlias)    ? 1
                : (R == AliasResult::MustAlias) ? 2
                                                : 0;
      Counts[idx]++;

      if (R != AliasResult::NoAlias) {
        OS << "  ";
        Pointers[i]->printAsOperand(OS, false);
        OS << " and ";
        Pointers[j]->printAsOperand(OS, false);
        OS << " -> " << Names[idx] << "\n";
      }
    }
  }

  OS << "\n--- Summary ---\n";
  for (int i = 0; i < 3; ++i)
    OS << Names[i] << ": " << Counts[i] << "\n";
}

} // namespace sparrow_aa
