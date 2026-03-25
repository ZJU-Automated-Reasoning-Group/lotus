#pragma once

#include <iostream>
#include <memory>

#ifndef NDEBUG
#include "Verification/SymAbsAI/Core/repr.h"

#include <csignal>
#endif

#ifdef DEBUG_TYPE
#undef DEBUG_TYPE
#endif
#define DEBUG_TYPE "symabs-ai"

#include <llvm/ADT/StringRef.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/Compiler.h>

// LLVM version compatibility - Lotus supports LLVM 14
#if LLVM_VERSION_MAJOR == 14
#define LLVM14 1
#else
#error Unsupported version of LLVM (only 14 is supported)
#endif

// Dynamic analysis is disabled in Lotus integration
#undef ENABLE_DYNAMIC

// forward declarations for llvm classes
namespace llvm {
class Value;
class Function;
class Module;
class Instruction;
} // namespace llvm

namespace symabs_ai {
using std::make_shared;
using std::make_unique;
using std::shared_ptr;
using std::unique_ptr;

// forward declarations
class AbstractValue;
class Analyzer;
class ConcreteState;
class Fragment;
class FragmentDecomposition;
class InstructionSemantics;
class Semantics;
class ValueMapping;
class PrettyPrinter;

extern std::ostream vout;
extern bool VerboseEnable;

struct VOutBlock {
  VOutBlock(const std::string &name) { vout << name << " {{{" << '\n'; }

  ~VOutBlock() { vout << '\n' << "}}}" << '\n'; }
};

[[noreturn]] void panic(const std::string &in);

std::string escapeJSON(const std::string &);
std::string escapeHTML(const std::string &);

/**
 * Retrieves source file path for a give LLVM function.
 *
 * Returns full path to an existing source file or an empty string if sources
 * cannot be found.
 */
std::string getFunctionSourcePath(const llvm::Function *);

unique_ptr<llvm::Module> loadModule(std::string file_name);

bool isInSSAForm(llvm::Function *function);
} // namespace symabs_ai

#undef DEBUG_TYPE
