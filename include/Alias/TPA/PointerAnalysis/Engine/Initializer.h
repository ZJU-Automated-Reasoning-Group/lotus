#pragma once

#include "Alias/TPA/PointerAnalysis/Engine/WorkList.h"
#include "Alias/TPA/PointerAnalysis/Support/Store.h"

namespace tpa {

class GlobalState;
class Memo;

// Seeds the data-flow engine with the initial analysis frontier.
//
// Initializer consumes the Store produced by global initialization and creates
// the first ProgramPoint (entry context + entry CFG node). It also models
// command-line/environment pointer roots so that analysis of programs using
// argv/envp starts from a conservative yet useful state.
class Initializer {
private:
  GlobalState &globalState;
  Memo &memo;

public:
  Initializer(GlobalState &g, Memo &m) : globalState(g), memo(m) {}

  // Returns a worklist pre-populated with the entry ProgramPoint.
  // Side effects:
  // - Updates Env with argv/envp roots when present.
  // - Inserts the initial Store into Memo at the entry ProgramPoint.
  ForwardWorkList runOnInitState(Store &&initStore);
};

} // namespace tpa
