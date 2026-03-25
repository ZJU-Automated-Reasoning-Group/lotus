#pragma once

#include <map>
#include <set>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/ConstantRange.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

// Optional precision boosters (wired from MKintPass when available)
namespace llvm {
class AAResults;
class MemorySSA;
} // namespace llvm

namespace kint {

// Forward declarations
struct crange;
using bbrange_t = DenseMap<const BasicBlock *, DenseMap<const Value *, crange>>;

struct crange : public ConstantRange {
  using ConstantRange::ConstantRange;

  crange(uint32_t bw);
  crange(const ConstantRange &cr);
  crange();

  static constexpr auto cmpRegion() {
    return ConstantRange::makeAllowedICmpRegion;
  }
};

class RangeAnalysis {
public:
  RangeAnalysis() = default;
  ~RangeAnalysis() = default;

  // Main analysis methods
  void range_analysis(
      Function &F, std::map<const Function *, bbrange_t> &func2range_info,
      const DenseMap<const BasicBlock *, SetVector<const BasicBlock *>>
          &backedges,
      std::map<const GlobalVariable *, crange> &global2range,
      std::map<const GlobalVariable *, SmallVector<crange, 4>> &garr2ranges,
      std::map<const Function *, crange> &func2ret_range,
      std::map<ICmpInst *, bool> &impossible_branches,
      std::set<GetElementPtrInst *> &gep_oob,
      const MapVector<Function *, std::vector<CallInst *>> &func2tsrc,
      const SetVector<StringRef> &callback_tsrc_fn, const DataLayout &DL,
      llvm::AAResults *AA = nullptr, llvm::MemorySSA *MSSA = nullptr);

  void analyze_one_bb_range(
      BasicBlock *bb, DenseMap<const Value *, crange> &cur_rng,
      std::map<const Function *, bbrange_t> &func2range_info,
      const DenseMap<const BasicBlock *, SetVector<const BasicBlock *>>
          &backedges,
      std::map<const GlobalVariable *, crange> &global2range,
      std::map<const GlobalVariable *, SmallVector<crange, 4>> &garr2ranges,
      std::set<GetElementPtrInst *> &gep_oob,
      std::map<ICmpInst *, bool> &impossible_branches,
      const MapVector<Function *, std::vector<CallInst *>> &func2tsrc,
      const SetVector<StringRef> &callback_tsrc_fn,
      std::map<const Function *, crange> &func2ret_range, const DataLayout &DL,
      llvm::AAResults *AA = nullptr, llvm::MemorySSA *MSSA = nullptr);

  void init_ranges(
      Module &M, std::map<const Function *, bbrange_t> &func2range_info,
      std::map<const Function *, crange> &func2ret_range,
      SetVector<Function *> &range_analysis_funcs,
      std::map<const GlobalVariable *, crange> &global2range,
      std::map<const GlobalVariable *, SmallVector<crange, 4>> &garr2ranges,
      const SetVector<Function *> &taint_funcs,
      const SetVector<StringRef> &callback_tsrc_fn);

  void print_all_ranges(
      const std::map<const Function *, crange> &func2ret_range,
      const std::map<const GlobalVariable *, crange> &global2range,
      const std::map<const GlobalVariable *, SmallVector<crange, 4>>
          &garr2ranges,
      const std::map<const Function *, bbrange_t> &func2range_info,
      const std::map<ICmpInst *, bool> &impossible_branches,
      const std::set<GetElementPtrInst *> &gep_oob) const;

private:
  crange
  get_range(const Value *var, const DenseMap<const Value *, crange> &brange,
            const std::map<const GlobalVariable *, crange> &global2range);

public:
  crange
  get_range_by_bb(const Value *var, const BasicBlock *bb,
                  const std::map<const Function *, bbrange_t> &func2range_info,
                  const std::map<const GlobalVariable *, crange> &global2range);

  crange compute_binary_rng(const BinaryOperator *op, const crange &lhs,
                            const crange &rhs);
};

} // namespace kint
