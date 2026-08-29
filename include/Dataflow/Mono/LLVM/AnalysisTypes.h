#ifndef LOTUS_DATAFLOW_MONO_LLVM_ANALYSISTYPES_H_
#define LOTUS_DATAFLOW_MONO_LLVM_ANALYSISTYPES_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"

#include "Dataflow/ControlFlow/InterCFG.h"
#include "Dataflow/ControlFlow/IntraCFG.h"
#include "Dataflow/Mono/Core/AbstractDomain.h"

#include <set>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace mono {

/**
 * @brief Solver-facing LLVM types for monotone dataflow analyses
 *
 * This is the standard domain type for LLVM-based analyses. It provides
 * sensible defaults for LLVM IR analysis:
 * - Instructions are CFG nodes (n_t)
 * - Values are dataflow facts (d_t, v_t)
 * - Functions are procedures (f_t)
 * - Module is the program database (db_t)
 *
 * **Usage:**
 * ```cpp
 * // Simple set-based analysis
 * using MyAnalysisTypes = LLVMMonoAnalysisTypes<Value*>;
 *
 * class MyProblem : public IntraMonoProblem<MyAnalysisTypes> {
 *   std::set<Value*> normalFlow(Instruction *Inst, const std::set<Value*> &In)
 * override {
 *     // Analysis implementation using std::set
 *   }
 * };
 * ```
 *
 * **Advanced usage with custom containers:**
 * ```cpp
 * // Bit-vector optimized for large universes
 * using MyAnalysisTypes = LLVMMonoAnalysisTypes<BitVectorSet<Instruction*>>;
 *
 * // Custom lattice type
 * using MyAnalysisTypes = LLVMMonoAnalysisTypes<std::map<Value*, ConstantValue>>;
 * ```
 *
 * @tparam ContainerType The container type for dataflow facts
 *         (e.g., std::set<Value*>, BitVectorSet<Instruction*>, custom map)
 */
template <typename ContainerType,
          typename AbstractDomainT = LegacyProblemDomain<ContainerType>>
struct LLVMMonoAnalysisTypes {
  // ========================================
  // Standard LLVM IR types
  // ========================================

  /// CFG node type (instructions)
  using n_t = llvm::Instruction *;

  /// Dataflow fact type (values)
  using d_t = llvm::Value *;

  /// Function/procedure type
  using f_t = llvm::Function *;

  /// Type system
  using t_t = llvm::Type *;

  /// Value type (same as d_t)
  using v_t = llvm::Value *;

  /// Program database (module)
  using db_t = llvm::Module;

  // ========================================
  // Control flow graph types
  // ========================================

  /// Intraprocedural CFG
  using c_t = ::dataflow::controlflow::IntraCFG;

  /// Interprocedural CFG
  using i_t = ::dataflow::controlflow::InterCFG;

  // ========================================
  // Pointer analysis
  // ========================================

  /// Points-to analysis / alias analysis
  using pt_t = lotus::AliasAnalysisWrapper *;

  // ========================================
  // Dataflow fact container
  // ========================================

  /**
   * @brief The container type used for storing dataflow facts
   *
   * This can be any type that supports:
   * - Construction/copy/move
   * - Iteration (for printing/debugging)
   * - Analysis-specific operations (union, intersection, etc.)
   *
   * Common choices:
   * - std::set<T> - general purpose, small universes
   * - BitVectorSet<T> - optimized for large fixed universes
   * - std::map<K, V> - for abstract domains with key-value mappings
   */
  using mono_container_t = ContainerType;
  using abstract_domain_t = AbstractDomainT;
};

// ============================================================================
// Common type aliases for convenience
// ============================================================================

/**
 * @brief Standard value-set domain (most common case)
 *
 * Use this for analyses that track sets of LLVM values:
 * - Live variables
 * - Reaching definitions
 * - Available expressions
 * - Taint analysis
 *
 * Example:
 * ```cpp
 * class LiveVariables : public IntraMonoProblem<ValueSetAnalysisTypes> {
 *   // Automatically uses std::set<Value*> as the fact container
 * };
 * ```
 */
using ValueSetAnalysisTypes = LLVMMonoAnalysisTypes<std::set<llvm::Value *>>;

/**
 * @brief Instruction-set domain
 *
 * Use this for analyses that track sets of instructions:
 * - Reaching definitions (instruction-based)
 * - Available expressions (instruction-based)
 *
 * Example:
 * ```cpp
 * class ReachingDefs : public IntraMonoProblem<InstructionSetAnalysisTypes> {
 *   // Uses std::set<Instruction*> as the fact container
 * };
 * ```
 */
using InstructionSetAnalysisTypes =
    LLVMMonoAnalysisTypes<std::set<llvm::Instruction *>>;

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_LLVM_ANALYSISTYPES_H_
