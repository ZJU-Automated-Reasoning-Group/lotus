// Implementation of the base Context class.
//
// A Context represents the calling history (call stack) of a function
// execution. In TPA, contexts are immutable and interned (deduplicated).
//
// Design:
// - Linked-List Structure: Each Context holds a reference to its predecessor
// (caller's context)
//   and the call instruction that created this context.
// - Interning: A global `ctxSet` stores all unique contexts. Pointers returned
// by
//   `pushContext` are unique representatives, allowing fast pointer equality
//   checks.
// - Global Context: A special context root for the entry point of the program.

#include "Alias/TPA/Context/Context.h"

#include "Alias/TPA/Context/ProgramPoint.h"

using namespace llvm;

namespace context {

// Pushes a new context frame onto the current context stack.
// This version takes a ProgramPoint, extracting the context and instruction
// from it.
const Context *Context::pushContext(const ProgramPoint &pp) {
  return pushContext(pp.getContext(), pp.getInstruction());
}

// Pushes a new context frame.
// Parameters:
//   ctx - The current (caller's) context.
//   inst - The call instruction (call site) invoking the function.
// Returns: A pointer to the unique interned Context object representing the new
// state.
const Context *Context::pushContext(const Context *ctx,
                                    const Instruction *inst) {
  // Create a temporary context object
  auto newCtx = Context(inst, ctx);
  // Insert into the set. If it exists, 'itr' points to the existing one.
  // If not, it points to the newly inserted one.
  auto itr = ctxSet.insert(newCtx).first;
  return &(*itr);
}

// Pops the most recent context frame.
// Returns the predecessor context (the caller's context).
const Context *Context::popContext(const Context *ctx) {
  assert(ctx->sz != 0 && "Trying to pop an empty context");
  return ctx->predContext;
}

// Retrieves the singleton Global Context.
// Used for global variables and the entry point of the analysis.
const Context *Context::getGlobalContext() {
  auto itr = ctxSet.insert(Context()).first;
  return &(*itr);
}

// Helper to retrieve all created contexts.
// Useful for debugging or statistics.
std::vector<const Context *> Context::getAllContexts() {
  std::vector<const Context *> ret;
  ret.reserve(ctxSet.size());

  for (auto const &ctx : ctxSet)
    ret.push_back(&ctx);

  return ret;
}

} // namespace context
