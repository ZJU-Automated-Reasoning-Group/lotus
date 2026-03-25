#include "llvm/Support/CommandLine.h"

namespace fitx {
namespace CommandLineArgs {
llvm::cl::opt<bool> Flex("flex", llvm::cl::desc("Print all possible errors"));
} // namespace CommandLineArgs
} // namespace fitx
