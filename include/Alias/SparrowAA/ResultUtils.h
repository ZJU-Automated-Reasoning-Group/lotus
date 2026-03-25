#ifndef TCFS_SPARROW_AA_RESULT_UTILS_H
#define TCFS_SPARROW_AA_RESULT_UTILS_H

#include <llvm/Support/raw_ostream.h>

namespace llvm {
class Value;
class Module;
} // namespace llvm

class Andersen;
class AndersenAAResult;

namespace sparrow_aa {

/// Print the points-to set for a given pointer value.
///
/// This function queries the Andersen analysis for the points-to set of the
/// given value and prints it in a human-readable format. The output includes
/// the number of locations the pointer points to and labels for each location
/// (global, stack, heap, or function).
///
/// \param V The pointer value to query
/// \param Anders The Andersen analysis instance
/// \param OS The output stream to write to
void printPointsToSet(const llvm::Value *V, Andersen &Anders,
                      llvm::raw_ostream &OS);

/// Perform and print alias queries between pointers in a module.
///
/// This function collects all pointer values in the module (globals and
/// instructions) and performs pairwise alias queries. It prints the results
/// and provides a summary of alias relationships (NoAlias, MayAlias,
/// MustAlias).
///
/// \param M The LLVM module to analyze
/// \param AAResult The Andersen alias analysis result
/// \param OS The output stream to write to
void performAliasQueries(llvm::Module &M, AndersenAAResult &AAResult,
                         llvm::raw_ostream &OS);

} // namespace sparrow_aa

#endif // TCFS_SPARROW_AA_RESULT_UTILS_H
