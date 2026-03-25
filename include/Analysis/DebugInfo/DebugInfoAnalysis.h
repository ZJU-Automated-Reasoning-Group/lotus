// Use debug info to better report bugs, e.g., line number, function name, etc.
// Adapted from a prior DebugInfoAnalysis implementation for LLVM 14+

#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm {
    class Instruction;
    class Value;
    class Function;
    class MDNode;
} // namespace llvm

class DebugInfoAnalysis {
public:
    DebugInfoAnalysis();
    ~DebugInfoAnalysis() = default;

    // Get source location information (file:line:col)
    std::string getSourceLocation(const llvm::Instruction *I);

    // Get function name from debug info or LLVM IR (with C++ demangling)
    std::string getFunctionName(const llvm::Instruction *I);

    // Get variable name from debug info or LLVM IR (with C++ demangling)
    std::string getVariableName(const llvm::Value *V);

    // Get type name as string
    std::string getTypeName(const llvm::Value *V);

    // Get source file path for a value
    std::string getSourceFile(const llvm::Value *V);

    // Get source line number
    int getSourceLine(const llvm::Value *V);

    // Get source column number
    int getSourceColumn(const llvm::Value *V);

    // Get the actual source code statement for an instruction.
    // Returns empty string if source file is not accessible.
    std::string getSourceCodeStatement(const llvm::Instruction *I);

    // Pre-populate the variable-name cache for all debug intrinsics in F.
    // Call this once per function to make subsequent getVariableName() O(1).
    void collectMetadata(const llvm::Function *F);

    // Return the MDNode (DILocalVariable) that describes V, or nullptr.
    // Searches F if provided, otherwise uses V's parent function.
    llvm::MDNode *findVarInfoMDNode(const llvm::Value *V,
                                    const llvm::Function *F = nullptr);

private:
    // Cache for source file contents (filename -> lines).
    // Shared across all instances; guarded by sourceFileCacheMutex in the .cpp.
    // Bounded to MAX_CACHE_FILES entries to limit memory usage.
    static std::map<std::string, std::vector<std::string>> sourceFileCache;

    // Helper: Read a source file into cache
    static bool loadSourceFile(const std::string& filepath);

    // Helper: Find the actual path to a source file
    static std::string findSourceFile(const std::string& filename);

    // Cache for variable names (per-instance, keyed by Value*)
    std::unordered_map<const llvm::Value*, std::string> varNameCache;

    // Internal recursive implementation of getVariableName with a depth guard
    std::string getVariableName(const llvm::Value *V, unsigned recursionDepth);
};
