

#include "Utils/LLVM/Debug.h"

#include <vector>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ManagedStatic.h>

using namespace llvm;

// Global debug flag for Popeye analysis.
bool PopeyeDebugFlag = false;

// Currently enabled debug types.
static ManagedStatic<std::vector<std::string>> PopeyeCurrentDebugType;

// Returns true if the given debug type is currently enabled.
bool isPopeyeCurrentDebugType(const char *DebugType) {
  if (PopeyeCurrentDebugType->empty())
    return true; // debug everything

  for (auto &D : *PopeyeCurrentDebugType)
    if (D == DebugType)
      return true;
  return false;
}

// Command line option handler for debug types.
struct PopeyeDebugOpt {
  void operator=(const std::string &Val) const {
    if (Val.empty())
      return;
    PopeyeDebugFlag = true;
    SmallVector<StringRef, 8> DbgTypes;
    StringRef(Val).split(DbgTypes, ',', -1, false);
    for (auto DbgType : DbgTypes)
      PopeyeCurrentDebugType->push_back(std::string(DbgType));
  }
};

// Global instance of the debug option handler.
static PopeyeDebugOpt DebugOptLoc;

static cl::opt<PopeyeDebugOpt, true, cl::parser<std::string>>
    DebugOnly("popeye-debug",
              cl::desc("Enable a specific type of debug output (comma "
                       "separated list of types)"),
              cl::Hidden, cl::ZeroOrMore, cl::value_desc("debug string"),
              cl::location(DebugOptLoc), cl::ValueRequired);
