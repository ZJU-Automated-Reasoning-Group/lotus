
#pragma once
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include <set>
#include <sstream>
#include <string>
#include <unordered_set>

using namespace llvm;

class InstructionUtils {
public:
  /***
   *  Is any of the operands to the instruction is a pointer?
   * @param I  Instruction to be checked.
   * @return  true/false
   */
  static bool isPointerInstruction(Instruction *I);

  /***
   *  Get the line number of the instruction.
   * @param I instruction whose line number need to be fetched.
   * @return unsigned int representing line number.
   */
  static unsigned getLineNumber(Instruction &I);

  /***
   *  Get the name of the provided instruction.
   * @param I instruction whose name needs to be fetched.
   * @return string representing the instruction name.
   */
  static std::string getInstructionName(Instruction *I);

  /***
   * Get the name of the provided value operand.
   * @param v The value operand whose name needs to be fetched.
   * @return string representing name of the provided value.
   */
  static std::string getValueName(Value *v);

  /***
   *  Method to convert string to be json friendly.
   *  Copied from:
   * https://stackoverflow.com/questions/7724448/simple-json-string-escape-for-c
   * @param input input string
   * @return converted string.
   */
  static std::string escapeJsonString(const std::string &input);

  /***
   * Method to convert the provided value to escaped json string.
   *
   * @param currInstr Value object which needs to be converted to json string.
   * @return Converted string.
   */
  static std::string escapeValueString(Value *currInstr);

  /***
   * Get the instruction line number corresponding to the provided instruction.
   * @param I Instruction whose line number needs to be fetched.
   * @return Line number.
   */
  static int getInstrLineNumber(Instruction *I);

  /***
   * Get the correct Debug Location (handles in lineing) for the provided
   * instruction.
   *
   * @param I instruction whose correct debug location needs to be fetched.
   * @return DILocation correct debug location corresponding to the provided
   * instruction.
   */
  static DILocation *getCorrectInstrLocation(Instruction *I);

  /***
   * Convert an LLVM Value to a printable string.
   * @param V The value to convert.
   * @return String representation of the value (truncated to 128 chars).
   */
  static std::string valueToString(const Value *V);

  /***
   * Convert an LLVM Type to a printable string.
   * @param T The type to convert.
   * @return String representation of the type (truncated to 128 chars).
   */
  static std::string typeToString(const Type *T);

  /***
   * Check if an instruction is known NOT to be defined by the user.
   * Returns true if the instruction is from system code (not in /home/
   * directory).
   * @param I Instruction to check.
   * @return true if system-defined, false otherwise.
   */
  static bool isSystemDefined(const Instruction *I);

  /***
   * Get fully inlined source location (walk inlined-at chain).
   * @param I Instruction to get source location for.
   * @return DILocation at the end of inline chain, or nullptr.
   */
  static DILocation *getFullyInlinedSrcLoc(const Instruction *I);

  /***
   * Get source row and column location if known (needs debug symbols).
   * @param I Instruction to get location for.
   * @return pair of {row, column}, or negative values if unknown.
   */
  static std::pair<int64_t, int64_t> getSourceLocation(const Instruction *I);

  /***
   * Get string with the name of the function & file where function is defined.
   * @param F Function to get location for.
   * @param ModID Module identifier string.
   * @return String in format "function;;filename;;moduleID".
   */
  static std::string getSourceLocationString(const Function *F,
                                             const std::string &ModID);

  /***
   * Check if instruction is a lifetime.start intrinsic.
   * @param I Instruction to check.
   * @return true if lifetime.start, false otherwise.
   */
  static bool isLifetimeStart(const Instruction *I);

  /***
   * Check if instruction is a lifetime.end intrinsic.
   * @param I Instruction to check.
   * @return true if lifetime.end, false otherwise.
   */
  static bool isLifetimeEnd(const Instruction *I);

  /***
   * Check if address marked dead is certain to never become alive again.
   * @param II Lifetime end intrinsic instruction.
   * @return true if address stays dead until function return.
   */
  static bool staysDead(IntrinsicInst *II);

  /***
   * Get set of all alloca instructions that could have allocated the lifetime
   * marker's address.
   * @param II Lifetime intrinsic instruction.
   * @return Set of alloca instructions.
   */
  static std::set<AllocaInst *> getAllocas(const IntrinsicInst *II);

  /***
   * Get underlying called function even if function is some sort of statepoint
   * instruction.
   * @param CB CallBase instruction.
   * @return Called function, or nullptr.
   */
  static Function *getCalledFunction(const CallBase *CB);

  /***
   * Determine if call instruction is possibly unsafe (unsafe if uncertain).
   * @param CB CallBase instruction to check.
   * @return true if possibly unsafe, false if definitely safe.
   */
  static bool isPossiblyUnsafe(const CallBase *CB);

  /***
   * Determine if function is possibly unsafe (unsafe if uncertain).
   * @param F Function to check.
   * @param FAM Optional function analysis manager for library info.
   * @return true if possibly unsafe, false if definitely safe.
   */
  static bool isPossiblyUnsafe(Function *F,
                               FunctionAnalysisManager *FAM = nullptr);

  /***
   * Determine whether memory operand access is statically known to be fully in
   * bounds & safe.
   * @param ObjSizeVis Object size visitor for computing object sizes.
   * @param Addr Address being accessed.
   * @param TySize Size of type being accessed (in bits).
   * @return true if definitely safe, false otherwise.
   */
  static bool isFullySafeAccess(ObjectSizeOffsetVisitor &ObjSizeVis,
                                Value *Addr, uint64_t TySize);

  /***
   * Get the set of known memory allocation/deallocation functions.
   * @return Set of function names.
   */
  static const std::unordered_set<std::string> &getKnownMemoryFunctions();
};
