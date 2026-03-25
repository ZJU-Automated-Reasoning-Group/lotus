/**
 * @file ThreadLocalAnalysis.h
 * @brief Thread-Local Storage (TLS) Detection
 *
 * This file provides utilities for detecting thread-local variables and allocations
 * that cannot participate in data races by definition.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#pragma once

#include <unordered_set>

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

namespace ThreadLocal {

/**
 * @class ThreadLocalAnalysis
 * @brief Identifies thread-local variables and storage
 *
 * Detects:
 * - C/C++ thread_local variables (global with TLS linkage)
 * - __thread variables (GCC/Clang extension)
 * - pthread_key_t thread-specific data
 * - Stack-allocated variables that don't escape their thread
 */
class ThreadLocalAnalysis {
public:
  explicit ThreadLocalAnalysis(llvm::Module &module);
  
  /**
   * @brief Run the analysis to identify all thread-local storage
   */
  void analyze();
  
  /**
   * @brief Check if a value is thread-local
   * @param val The value to check
   * @return true if the value is definitely thread-local
   */
  bool isThreadLocal(const llvm::Value *val) const;
  
  /**
   * @brief Check if an instruction accesses thread-local storage
   * @param inst The instruction to check
   * @return true if the instruction accesses only thread-local storage
   */
  bool accessesThreadLocalStorage(const llvm::Instruction *inst) const;
  
  /**
   * @brief Get all thread-local global variables
   * @return Set of thread-local globals
   */
  const std::unordered_set<const llvm::GlobalVariable *> &getThreadLocalGlobals() const {
    return m_tls_globals;
  }
  
  /**
   * @brief Get all thread-local allocations
   * @return Set of thread-local alloca instructions
   */
  const std::unordered_set<const llvm::AllocaInst *> &getThreadLocalAllocas() const {
    return m_tls_allocas;
  }

private:
  llvm::Module &m_module;
  
  // Identified thread-local storage
  std::unordered_set<const llvm::GlobalVariable *> m_tls_globals;
  std::unordered_set<const llvm::AllocaInst *> m_tls_allocas;
  std::unordered_set<const llvm::Value *> m_tls_values;
  
  // pthread_key_t thread-specific data keys
  std::unordered_set<const llvm::Value *> m_pthread_keys;
  
  /**
   * @brief Identify thread-local global variables
   */
  void identifyThreadLocalGlobals();
  
  /**
   * @brief Identify thread-local stack allocations
   */
  void identifyThreadLocalAllocas();
  
  /**
   * @brief Identify pthread thread-specific data
   */
  void identifyPthreadSpecificData();
  
  /**
   * @brief Check if a global variable has TLS linkage
   */
  static bool hasThreadLocalStorageLinkage(const llvm::GlobalVariable *gv);
  
  /**
   * @brief Check if an alloca is thread-local (doesn't escape)
   */
  bool isAllocaThreadLocal(const llvm::AllocaInst *alloca) const;
  
  /**
   * @brief Check if a value escapes its thread
   */
  bool escapesThread(const llvm::Value *val) const;
};

/**
 * @brief Check if a value is thread-local without full analysis
 * @param val The value to check
 * @return true if the value is definitely thread-local (fast check)
 */
bool isObviouslyThreadLocal(const llvm::Value *val);

} // namespace ThreadLocal
