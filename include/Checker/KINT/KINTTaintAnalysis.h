#pragma once

#include <string>
#include <vector>

#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace kint {

class TaintAnalysis {
public:
  TaintAnalysis() = default;
  ~TaintAnalysis() = default;

  // Taint source identification
  static bool is_taint_src(StringRef sv);
  std::vector<CallInst *> get_taint_source(Function &F);
  static bool is_taint_src_arg_call(StringRef s);

  // Sink identification and marking
  void mark_func_sinks(Function &F, SetVector<StringRef> &callback_tsrc_fn);
  static SmallVector<Function *, 2> get_sink_fns(Instruction *inst);

  // Taint propagation
  bool is_sink_reachable(Instruction *inst, SetVector<Function *> &taint_funcs);
  bool taint_bcast_sink(Function *f,
                        const std::vector<CallInst *> &taint_source,
                        SetVector<Function *> &taint_funcs);
  template <typename Iter>
  bool taint_bcast_sink(Iter taint_source, SetVector<Function *> &taint_funcs);

  // Cross-function taint propagation
  void propagate_taint_across_functions(
      Module &M, MapVector<Function *, std::vector<CallInst *>> &func2tsrc,
      SetVector<Function *> &taint_funcs);

private:
  static void mark_taint(Instruction &inst, const std::string &taint_name = "");
};

} // namespace kint
