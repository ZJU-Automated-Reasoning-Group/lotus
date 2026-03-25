/*
 * CLI utilities for alias analysis tools
 *
 * This file provides shared functionality for loading IR modules and
 * managing pointer analysis configuration files for command-line tools.
 */

#ifndef ALIAS_ALIASANALYSISWRAPPER_CLIUTILS_H
#define ALIAS_ALIASANALYSISWRAPPER_CLIUTILS_H

#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SourceMgr.h>

namespace llvm {
class Module;
class LLVMContext;
class SMDiagnostic;
} // namespace llvm

namespace lotus {
namespace alias {

class AliasSpecManager;

namespace tools {

/// Load an LLVM IR module from a file
///
/// \param Filename Path to the input bitcode file
/// \param Context LLVM context to use
/// \param Err Diagnostic object for error reporting
/// \param ProgramName Name of the program (for error messages)
/// \returns Unique pointer to the loaded module, or nullptr on error
std::unique_ptr<llvm::Module> loadIRModule(const std::string &Filename,
                                           llvm::LLVMContext &Context,
                                           llvm::SMDiagnostic &Err,
                                           const char *ProgramName = nullptr);

/// Command-line options for pointer analysis configuration files
extern llvm::cl::opt<std::string> ConfigFile;
extern llvm::cl::list<std::string> ConfigFiles;

/// Collect configuration file paths from command-line options
///
/// This function collects paths from both -config (comma-separated) and
/// -config-file (list) options.
///
/// \returns Vector of configuration file paths
std::vector<std::string> collectConfigFilePaths();

/// Create and initialize an AliasSpecManager with configuration files
///
/// \param SpecFilePaths Vector of paths to spec files (e.g., ptr.spec)
/// \param Module Optional module to initialize the spec manager with
/// \returns Unique pointer to the initialized AliasSpecManager
///
/// If SpecFilePaths is empty, creates a manager with default configuration.
/// If Module is provided, initializes the manager with the module for
/// better name matching.
std::unique_ptr<::lotus::alias::AliasSpecManager>
createAliasSpecManager(const std::vector<std::string> &SpecFilePaths = {},
                       llvm::Module *Module = nullptr);

/// Print loaded configuration files to stderr
///
/// \param SpecManager The AliasSpecManager to query
void printLoadedConfigFiles(
    const ::lotus::alias::AliasSpecManager &SpecManager);

} // namespace tools
} // namespace alias
} // namespace lotus

#endif // ALIAS_ALIASANALYSISWRAPPER_CLIUTILS_H
