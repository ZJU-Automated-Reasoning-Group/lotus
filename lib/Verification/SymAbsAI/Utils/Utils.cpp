/**
 * @file Utils.cpp
 * @brief Utility functions and global state for the SymAbsAI module.
 *
 * Provides utility functions for string escaping, module loading, SSA form
 * checking, and debug information extraction. Also implements the global
 * verbose output stream that can be enabled/disabled for debugging.
 *
 * @author rainoftime
 */
#include "Verification/SymAbsAI/Utils/Utils.h"

#include "Verification/SymAbsAI/Core/repr.h"

#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>

namespace symabs_ai {
namespace // unnamed
{
/**
 * @brief String buffer that conditionally outputs to stderr based on
 * VerboseEnable.
 *
 * Used to implement the global vout stream that only prints when verbose mode
 * is enabled.
 */
class VerboseOutBuf : public std::stringbuf {
protected:
  virtual int sync() {
    if (VerboseEnable)
      std::cerr << str();

    str("");
    return 0;
  }
} VerboseOutBufInstance;
} // namespace

/** @brief Global flag controlling verbose output. When true, vout prints to
 * stderr. */
bool VerboseEnable;
/** @brief Global verbose output stream. Only outputs when VerboseEnable is
 * true. */
std::ostream vout(&VerboseOutBufInstance);

/**
 * @brief Print an error message and exit the program.
 *
 * @param in Error message to print before exiting
 */
void panic(const std::string &in) {
  std::cerr << "Error: " << in << '\n';
  exit(13);
}

/**
 * @brief Escape special characters in a string for JSON encoding.
 *
 * Escapes control characters (< 0x20), backslashes, and double quotes using
 * Unicode escape sequences (\uXXXX format).
 *
 * @param in Input string to escape
 * @return JSON-escaped string
 */
std::string escapeJSON(const std::string &in) {
  std::stringstream ss;

  for (unsigned c : in) {
    if (c < '\x20' || c == '\\' || c == '"')
      ss << "\\u" << std::setfill('0') << std::setw(4) << std::hex << c;
    else
      ss << (char)c;
  }

  return ss.str();
}

/**
 * @brief Escape special characters in a string for HTML encoding.
 *
 * Escapes HTML special characters: < becomes &lt;, > becomes &gt;, & becomes
 * &amp;. Non-printable characters are removed from the output.
 *
 * @param in Input string to escape
 * @return HTML-escaped string
 */
std::string escapeHTML(const std::string &in) {
  std::stringstream ss;

  for (char c : in) {
    switch (c) {
    case '<':
      ss << "&lt;";
      break;

    case '>':
      ss << "&gt;";
      break;

    case '&':
      ss << "&amp;";
      break;

    default:
      if (std::isprint(c))
        ss << (char)c;
    }
  }

  return ss.str();
}

/**
 * @brief Extract the source file path for a function from debug information.
 *
 * Searches through the function's instructions to find debug location
 * information and constructs the full path by combining directory and filename.
 *
 * @param function The LLVM function to get source path for
 * @return Source file path if found, empty string otherwise
 */
std::string getFunctionSourcePath(const llvm::Function *function) {
  // Try to get debug info from the first instruction
  for (const auto &bb : *function) {
    for (const auto &inst : bb) {
      if (const llvm::DILocation *debugLoc = inst.getDebugLoc()) {
        if (debugLoc->getFile()) {
          llvm::SmallString<256> path = debugLoc->getDirectory();
          llvm::sys::path::append(path, debugLoc->getFilename());
          return path.str().str();
        }
      }
    }
  }
  return "";
}

/**
 * @brief Load an LLVM bitcode module from a file.
 *
 * Reads a bitcode file from disk and parses it into an LLVM Module object.
 * Throws std::runtime_error if the file cannot be read or parsed.
 *
 * @param file_name Path to the bitcode file to load
 * @return Unique pointer to the loaded Module
 * @throws std::runtime_error if file cannot be loaded or parsed
 */
unique_ptr<llvm::Module> loadModule(std::string file_name) {
  auto err_or_file = llvm::MemoryBuffer::getFile(file_name);

  if (!err_or_file)
    throw std::runtime_error("Cannot load file: `" + file_name + "'");

  llvm::LLVMContext context;
  auto err_or_module =
      llvm::parseBitcodeFile(err_or_file.get()->getMemBufferRef(), context);

  if (!err_or_module)
    throw std::runtime_error("Cannot parse bitcode file: `" + file_name + "'");

  return std::move(err_or_module.get());
}

/**
 * @brief Check if a function is in Static Single Assignment (SSA) form.
 *
 * A function is considered to be in SSA form if all promotable alloca
 * instructions have been promoted to registers. This checks for any
 * remaining promotable allocas that would indicate the function is not
 * in SSA form.
 *
 * @param function The LLVM function to check
 * @return true if the function is in SSA form, false otherwise
 */
bool isInSSAForm(llvm::Function *function) {
  using namespace llvm;

  for (BasicBlock &bb : *function) {
    for (Instruction &inst : bb) {
      if (auto *ai = dyn_cast<AllocaInst>(&inst)) {
        if (isAllocaPromotable(ai))
          return false;
      }
    }
  }

  return true;
}
} // end namespace symabs_ai
