#include "Checker/KINT/RangeAnalysis.h"

#include "Checker/KINT/KINTTaintAnalysis.h"
#include "Checker/KINT/Log.h"
#include "Checker/KINT/Options.h"
#include "Utils/Types/range.h"

#include <deque>

#include <llvm/Analysis/MemorySSA.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace kint {

// crange implementation
crange::crange(uint32_t bw) : ConstantRange(bw, true) {}
crange::crange(const ConstantRange &cr) : ConstantRange(cr) {}
crange::crange() : ConstantRange(0, true) {}

/// Normalize a range to \p targetBits so it can be used in binary ops (which
/// require both operands to have the same bit width).
static crange alignToBitWidth(const crange &r, uint32_t targetBits) {
  const uint32_t bw = r.getBitWidth();
  if (bw == targetBits)
    return r;
  if (bw == 0)
    return crange(targetBits, true); // full range when unknown
  if (targetBits == 0)
    return crange(0, true); // cannot truncate to 0
  if (bw < targetBits)
    return r.zeroExtend(targetBits);
  return r.truncate(targetBits);
}

crange RangeAnalysis::compute_binary_rng(const BinaryOperator *op,
                                         const crange &lhs, const crange &rhs) {
  if (!op->getType()->isIntegerTy()) {
    MKINT_LOG() << "Binary op with non-integer result type";
    return crange(0, true);
  }
  const uint32_t resultBits = op->getType()->getIntegerBitWidth();
  crange l = alignToBitWidth(lhs, resultBits);
  crange r = alignToBitWidth(rhs, resultBits);

  switch (op->getOpcode()) {
  case Instruction::Add:
    return l.add(r);
  case Instruction::Sub:
    return l.sub(r);
  case Instruction::Mul:
    return l.multiply(r);
  case Instruction::UDiv:
    return l.udiv(r);
  case Instruction::SDiv:
    return l.sdiv(r);
  case Instruction::Shl:
    return l.shl(r);
  case Instruction::LShr:
    return l.lshr(r);
  case Instruction::AShr:
    return l.ashr(r);
  case Instruction::And:
    return l.binaryAnd(r);
  case Instruction::Or:
    return l.binaryOr(r);
  case Instruction::Xor:
    return l.binaryXor(r);
  case Instruction::URem:
    return l.urem(r);
  case Instruction::SRem:
    return l.srem(r);
  default:
    MKINT_LOG() << "Unhandled binary opcode: " << op->getOpcodeName();
  }
  return r;
}

crange RangeAnalysis::get_range(
    const Value *var, const DenseMap<const Value *, crange> &brange,
    const std::map<const GlobalVariable *, crange> &global2range) {
  auto it = brange.find(var);
  if (it != brange.end()) {
    return it->second;
  }

  if (const auto *lconst = dyn_cast<ConstantInt>(var)) {
    return crange(lconst->getValue());
  } else {
    if (const auto *gv = dyn_cast<GlobalVariable>(var)) {
      auto it = global2range.find(gv);
      if (it != global2range.end()) {
        return it->second;
      }
    }
  }
  MKINT_WARN() << "Unknown operand type: " << *var;
  return crange(var->getType()->getIntegerBitWidth(), true);
}

crange RangeAnalysis::get_range_by_bb(
    const Value *var, const BasicBlock *bb,
    const std::map<const Function *, bbrange_t> &func2range_info,
    const std::map<const GlobalVariable *, crange> &global2range) {
  auto func_it = func2range_info.find(bb->getParent());
  if (func_it == func2range_info.end()) {
    return crange(var->getType()->getIntegerBitWidth(), true);
  }
  auto bb_it = func_it->second.find(bb);
  if (bb_it == func_it->second.end()) {
    return crange(var->getType()->getIntegerBitWidth(), true);
  }
  return get_range(var, bb_it->second, global2range);
}

void RangeAnalysis::analyze_one_bb_range(
    BasicBlock *bb, DenseMap<const Value *, crange> &cur_rng,
    std::map<const Function *, bbrange_t> &func2range_info,
    const DenseMap<const BasicBlock *, SetVector<const BasicBlock *>>
        &backedges,
    std::map<const GlobalVariable *, crange> &global2range,
    std::map<const GlobalVariable *, SmallVector<crange, 4>> &garr2ranges,
    std::set<GetElementPtrInst *> &gep_oob,
    std::map<ICmpInst *, bool> & /*impossible_branches*/,
    const MapVector<Function *, std::vector<CallInst *>> &func2tsrc,
    const SetVector<StringRef> & /*callback_tsrc_fn*/,
    std::map<const Function *, crange> &func2ret_range, const DataLayout &DL,
    llvm::AAResults * /*AA*/, llvm::MemorySSA *MSSA) {
  auto &F = *bb->getParent();
  auto &sum_rng = func2range_info[&F][bb];

  for (auto &inst : bb->getInstList()) {
    const auto get_rng = [&cur_rng, &global2range, this](auto var) {
      return get_range(var, cur_rng, global2range);
    };

    // Store / Call / Return
    if (auto *const call = dyn_cast<CallInst>(&inst)) {
      if (auto *const f = call->getCalledFunction()) {
        // Skip functions without a valid entry block (declarations or corrupted
        // state)
        if (f->isDeclaration() || f->empty())
          continue;

        if (func2tsrc.find(f) != func2tsrc.end()) {
          const auto &argcalls = func2tsrc.find(f)->second;

          for (const auto &arg : f->args()) {
            auto &argblock = func2range_info[f][&(f->getEntryBlock())];
            const size_t arg_idx = arg.getArgNo();
            if (arg.getType()->isIntegerTy()) {
              auto arg_rng = get_rng(call->getArgOperand(arg_idx));
              if (argblock.count(&arg) &&
                  argblock[&arg].getBitWidth() == arg_rng.getBitWidth()) {
                argblock[&arg] = arg_rng.unionWith(argblock[&arg]);
              } else {
                argblock[&arg] = arg_rng;
              }
              func2ret_range[argcalls[arg_idx]->getCalledFunction()] =
                  argblock[&arg];
            }
          }
        } else {
          for (const auto &arg : f->args()) {
            auto &argblock = func2range_info[f][&(f->getEntryBlock())];
            if (arg.getType()->isIntegerTy()) {
              auto arg_rng = get_rng(call->getArgOperand(arg.getArgNo()));
              if (argblock.count(&arg) &&
                  argblock[&arg].getBitWidth() == arg_rng.getBitWidth()) {
                argblock[&arg] = arg_rng.unionWith(argblock[&arg]);
              } else {
                argblock[&arg] = arg_rng;
              }
            }
          }
        }

        if (f->getReturnType()->isIntegerTy()) // return value is integer.
          cur_rng[call] = func2ret_range[f];
      }

      continue;
    } else if (auto *const store = dyn_cast<StoreInst>(&inst)) {
      // is global var
      auto *const val = store->getValueOperand();
      auto *const ptr = store->getPointerOperand();

      if (!val->getType()->isIntegerTy())
        continue;

      auto valrng = get_rng(val);
      if (auto *const gv = dyn_cast<GlobalVariable>(ptr)) {
        if (global2range.count(gv) &&
            global2range[gv].getBitWidth() == valrng.getBitWidth()) {
          global2range[gv] = global2range[gv].unionWith(valrng);
        } else {
          global2range[gv] = valrng;
        }
      } else if (auto *const gep = dyn_cast<GetElementPtrInst>(ptr)) {
        auto *gep_addr = gep->getPointerOperand();
        if (auto *garr = dyn_cast<GlobalVariable>(gep_addr)) {
          if (garr2ranges.count(garr) &&
              gep->getNumIndices() == 2) { // all one dim array<int>s!
            auto *idx = gep->getOperand(2);
            const size_t arr_size = garr2ranges[garr].size();
            const crange idx_rng = get_rng(idx);
            const size_t idx_max = idx_rng.getUnsignedMax().getLimitedValue();
            if (CheckArrayOOB && !RobustReachability && idx_max >= arr_size)
              gep_oob.insert(gep);

            // Use <= idx_max (clamped to arr_size-1) so the last reachable
            // element is included in the range merge.
            const size_t idx_min = idx_rng.getUnsignedMin().getLimitedValue();
            const size_t idx_end =
                std::min(arr_size, idx_max + 1); // inclusive upper bound
            for (size_t i = idx_min; i < idx_end; ++i) {
              if (garr2ranges[garr][i].getBitWidth() == valrng.getBitWidth()) {
                garr2ranges[garr][i] = garr2ranges[garr][i].unionWith(valrng);
              } else {
                garr2ranges[garr][i] = valrng;
              }
            }
          }
        }

        // Stack arrays: alloca [N x T] with GEP 0, idx
        const Value *underlying = llvm::getUnderlyingObject(gep_addr);
        if (CheckArrayOOB) {
          if (const auto *ai = dyn_cast_or_null<AllocaInst>(underlying)) {
            if (const auto *at = dyn_cast<ArrayType>(ai->getAllocatedType())) {
              if (gep->getNumIndices() == 2) {
                const uint64_t arr_size = at->getNumElements();
                auto *idx = gep->getOperand(2);
                const crange idx_rng = get_rng(idx);
                const uint64_t idx_max =
                    idx_rng.getUnsignedMax().getLimitedValue();
                if (!RobustReachability && idx_max >= arr_size) {
                  gep_oob.insert(gep);
                }
              }
            }
          }
        }

        // Heap arrays: malloc-like objects indexed via GEP (best-effort,
        // range-based).
        if (CheckArrayOOB) {
          if (const auto *ci = dyn_cast_or_null<CallInst>(underlying)) {
            if (auto *f = ci->getCalledFunction()) {
              const auto name = f->getName();
              if ((name == "malloc" || name == "kmalloc" || name == "kzalloc" ||
                   name == "vmalloc") &&
                  ci->arg_size() >= 1) {
                const crange size_rng =
                    get_range_by_bb(ci->getArgOperand(0), ci->getParent(),
                                    func2range_info, global2range);
                const uint64_t size_min =
                    size_rng.getUnsignedMin().getLimitedValue();
                if (gep->getNumIndices() >= 1) {
                  auto *idx = gep->getOperand(gep->getNumOperands() - 1);
                  const crange idx_rng = get_rng(idx);
                  const uint64_t idx_max =
                      idx_rng.getUnsignedMax().getLimitedValue();
                  const uint64_t elem_bytes =
                      DL.getTypeAllocSize(gep->getResultElementType());
                  if (!RobustReachability && elem_bytes != 0 &&
                      idx_max * elem_bytes >= size_min) {
                    gep_oob.insert(gep);
                  }
                }
              }
            }
          }
        }
      }

      // Track local memory coarsely when possible:
      // - Prefer the underlying object when it is a stack/global allocation.
      // - Fall back to the pointer SSA value otherwise (may be aliased).
      const Value *underlying = llvm::getUnderlyingObject(ptr);
      if (isa<AllocaInst>(underlying) || isa<GlobalVariable>(underlying)) {
        cur_rng[underlying] = valrng;
      } else {
        cur_rng[ptr] = valrng;
      }
      continue;
    } else if (auto *const ret = dyn_cast<ReturnInst>(&inst)) {
      // low precision: just apply!
      if (F.getReturnType()->isIntegerTy()) {
        auto ret_rng = get_rng(ret->getReturnValue());
        if (func2ret_range.count(&F) &&
            func2ret_range[&F].getBitWidth() == ret_rng.getBitWidth()) {
          func2ret_range[&F] = ret_rng.unionWith(func2ret_range[&F]);
        } else {
          func2ret_range[&F] = ret_rng;
        }
      }

      continue;
    }

    // return type should be int
    if (!inst.getType()->isIntegerTy()) {
      continue;
    }

    // empty range
    crange new_range = crange::getEmpty(inst.getType()->getIntegerBitWidth());

    if (const BinaryOperator *op = dyn_cast<BinaryOperator>(&inst)) {
      auto *lhs = op->getOperand(0);
      auto *rhs = op->getOperand(1);

      crange lhs_range = get_rng(lhs), rhs_range = get_rng(rhs);
      new_range = compute_binary_rng(op, lhs_range, rhs_range);
    } else if (const SelectInst *op = dyn_cast<SelectInst>(&inst)) {
      const auto *const tval = op->getTrueValue();
      const auto *const fval = op->getFalseValue();
      auto tval_rng = get_rng(tval);
      auto fval_rng = get_rng(fval);
      if (tval_rng.getBitWidth() == fval_rng.getBitWidth()) {
        new_range = tval_rng.unionWith(fval_rng);
      } else {
        // If bit widths don't match, use the range with the correct bit width
        new_range =
            (tval_rng.getBitWidth() == inst.getType()->getIntegerBitWidth())
                ? tval_rng
                : fval_rng;
      }
    } else if (const CastInst *op = dyn_cast<CastInst>(&inst)) {
      new_range = [op, &get_rng]() -> crange {
        auto *inp = op->getOperand(0);
        if (!inp->getType()->isIntegerTy())
          return crange(op->getType()->getIntegerBitWidth(), true);
        auto inprng = get_rng(inp);
        const uint32_t bits = op->getType()->getIntegerBitWidth();
        switch (op->getOpcode()) {
        case CastInst::Trunc:
          if (inprng.getBitWidth() > bits)
            return inprng.truncate(bits);
          if (inprng.getBitWidth() < bits)
            return crange(bits, true); // defensive: source narrower than trunc
          return inprng;
        case CastInst::ZExt:
          if (inprng.getBitWidth() < bits)
            return inprng.zeroExtend(bits);
          if (inprng.getBitWidth() > bits)
            return inprng.truncate(bits);
          return inprng;
        case CastInst::SExt:
          if (inprng.getBitWidth() < bits)
            return inprng.signExtend(bits);
          if (inprng.getBitWidth() > bits)
            return inprng.truncate(bits);
          return inprng;
        default:
          MKINT_LOG() << "Unhandled Cast Instruction " << op->getOpcodeName()
                      << ". Using original range.";
        }
        return inprng;
      }();
    } else if (const PHINode *op = dyn_cast<PHINode>(&inst)) {
      for (size_t i = 0; i < op->getNumIncomingValues(); ++i) {
        auto *pred = op->getIncomingBlock(i);
        auto bb_it = backedges.find(bb);
        if (bb_it != backedges.end() && bb_it->second.contains(pred)) {
          continue; // skip backedge
        }
        auto incoming_rng = get_range_by_bb(op->getIncomingValue(i), pred,
                                            func2range_info, global2range);
        if (new_range.getBitWidth() == incoming_rng.getBitWidth()) {
          new_range = new_range.unionWith(incoming_rng);
        } else if (new_range.isEmptySet()) {
          new_range = incoming_rng;
        }
      }
    } else if (auto *op = dyn_cast<LoadInst>(&inst)) {
      auto *addr = op->getPointerOperand();
      bool resolved = false;

      // If MemorySSA is available, prefer the clobbering store's value (when it
      // is a simple store of an int).
      if (MSSA) {
        if (auto *acc = MSSA->getMemoryAccess(op)) {
          if (auto *use = dyn_cast<llvm::MemoryUse>(acc)) {
            if (auto *def = dyn_cast_or_null<llvm::MemoryDef>(
                    use->getDefiningAccess())) {
              if (auto *defInst = def->getMemoryInst()) {
                if (auto *clobberStore = dyn_cast<StoreInst>(defInst)) {
                  auto *storedVal = clobberStore->getValueOperand();
                  if (storedVal && storedVal->getType()->isIntegerTy()) {
                    new_range = get_rng(storedVal);
                    resolved = true;
                  }
                }
              }
            }
          }
        }
      }

      if (!resolved) {
        // If we have a locally tracked value for the address (e.g., alloca or
        // underlying object), use it.
        if (cur_rng.count(addr)) {
          new_range = get_rng(addr);
          resolved = true;
        } else {
          const Value *underlying = llvm::getUnderlyingObject(addr);
          if (underlying && cur_rng.count(underlying)) {
            new_range = get_rng(underlying);
            resolved = true;
          } else if (isa<GlobalVariable>(addr)) {
            new_range = get_rng(addr);
            resolved = true;
          }
        }
      }

      if (!resolved) {
        if (auto *gep = dyn_cast<GetElementPtrInst>(addr)) {
          bool succ = false;
          // we only analyze shallow arrays. i.e., one dim.
          auto *gep_addr = gep->getPointerOperand();
          if (auto *garr = dyn_cast<GlobalVariable>(gep_addr)) {
            if (garr2ranges.count(garr) &&
                gep->getNumIndices() == 2) { // all one dim array<int>s!
              auto *idx = gep->getOperand(2);
              const size_t arr_size = garr2ranges[garr].size();
              const crange idx_rng = get_rng(idx);
              const size_t idx_max = idx_rng.getUnsignedMax().getLimitedValue();
              if (CheckArrayOOB && !RobustReachability && idx_max >= arr_size) {
                gep_oob.insert(gep);
              }

              // Include idx_max itself (clamped to arr_size-1) so the last
              // reachable element contributes to the merged range.
              const size_t load_idx_min =
                  idx_rng.getUnsignedMin().getLimitedValue();
              const size_t load_idx_end = std::min(arr_size, idx_max + 1);
              for (size_t i = load_idx_min; i < load_idx_end; ++i) {
                if (new_range.getBitWidth() ==
                    garr2ranges[garr][i].getBitWidth()) {
                  new_range = new_range.unionWith(garr2ranges[garr][i]);
                } else if (new_range.isEmptySet()) {
                  new_range = garr2ranges[garr][i];
                }
              }

              succ = true;
            }
          }

          // Stack arrays: alloca [N x T] with GEP 0, idx (best-effort).
          if (!succ) {
            const Value *underlying = llvm::getUnderlyingObject(gep_addr);
            if (const auto *ai = dyn_cast_or_null<AllocaInst>(underlying)) {
              if (const auto *at =
                      dyn_cast<ArrayType>(ai->getAllocatedType())) {
                if (gep->getNumIndices() == 2) {
                  const uint64_t arr_size = at->getNumElements();
                  auto *idx = gep->getOperand(2);
                  const crange idx_rng = get_rng(idx);
                  const uint64_t idx_max =
                      idx_rng.getUnsignedMax().getLimitedValue();
                  if (CheckArrayOOB && !RobustReachability &&
                      idx_max >= arr_size) {
                    gep_oob.insert(gep);
                  }
                  new_range = crange(op->getType()->getIntegerBitWidth(), true);
                  succ = true;
                }
              }
            }
          }

          // Heap arrays: malloc-like objects indexed via GEP (best-effort,
          // range-based).
          if (!succ) {
            const Value *underlying = llvm::getUnderlyingObject(gep_addr);
            if (const auto *ci = dyn_cast_or_null<CallInst>(underlying)) {
              if (auto *f = ci->getCalledFunction()) {
                const auto name = f->getName();
                if ((name == "malloc" || name == "kmalloc" ||
                     name == "kzalloc" || name == "vmalloc") &&
                    ci->arg_size() >= 1) {
                  const crange size_rng =
                      get_range_by_bb(ci->getArgOperand(0), ci->getParent(),
                                      func2range_info, global2range);
                  const uint64_t size_min =
                      size_rng.getUnsignedMin().getLimitedValue();
                  auto *idx = gep->getOperand(gep->getNumOperands() - 1);
                  const crange idx_rng = get_rng(idx);
                  const uint64_t idx_max =
                      idx_rng.getUnsignedMax().getLimitedValue();
                  const uint64_t elem_bytes =
                      DL.getTypeAllocSize(gep->getResultElementType());
                  if (CheckArrayOOB && !RobustReachability && elem_bytes != 0 &&
                      idx_max * elem_bytes >= size_min) {
                    gep_oob.insert(gep);
                  }
                  new_range = crange(op->getType()->getIntegerBitWidth(), true);
                  succ = true;
                }
              }
            }
          }

          if (!succ) {
            MKINT_WARN() << "Unknown address to load (unknow gep src addr): "
                         << inst;
            new_range = crange(op->getType()->getIntegerBitWidth(),
                               true); // unknown addr -> full range.
          }
        } else {
          MKINT_WARN() << "Unknown address to load: " << inst;
          new_range =
              crange(op->getType()
                         ->getIntegerBitWidth()); // unknown addr -> full range.
        }
      }
    } else if (auto *const op = dyn_cast<CmpInst>(&inst)) {
      // can be more precise by comparing the range...
      // but nah...
    } else {
      MKINT_CHECK_RELAX(false)
          << " [Range Analysis] Unhandled instruction: " << inst;
    }

    // Check if cur_rng[&inst] exists and has the right bit width before
    // unionWith
    if (cur_rng.count(&inst) &&
        cur_rng[&inst].getBitWidth() == new_range.getBitWidth()) {
      cur_rng[&inst] = new_range.unionWith(cur_rng[&inst]);
    } else {
      cur_rng[&inst] = new_range;
    }
  }

  if (&cur_rng != &sum_rng) {
    for (auto &bb_rng_pair : cur_rng) {
      const auto *bb = bb_rng_pair.first;
      auto rng = bb_rng_pair.second;
      if (sum_rng.count(bb)) {
        if (sum_rng[bb].getBitWidth() == rng.getBitWidth()) {
          sum_rng[bb] = sum_rng[bb].unionWith(rng);
        } else {
          sum_rng[bb] = rng;
        }
      } else {
        sum_rng[bb] = rng;
      }
    }
  }
}

void RangeAnalysis::range_analysis(
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
    llvm::AAResults *AA, llvm::MemorySSA *MSSA) {
  MKINT_LOG() << "Range Analysis -> " << F.getName();

  auto &bb_range = func2range_info[&F];

  for (auto &bbref : F) {
    auto *bb = &bbref;
    auto &sum_rng = bb_range[bb];

    // merge all incoming bbs
    for (const auto &pred : predecessors(bb)) {
      // avoid backedge: pred can't be a successor of bb.
      auto bb_it = backedges.find(bb);
      if (bb_it != backedges.end() && bb_it->second.contains(pred))
        continue; // skip backedge

      MKINT_LOG() << "Merging: " << pred->getName() << "\t -> "
                  << bb->getName();
      auto branch_rng = bb_range[pred];
      auto *terminator = pred->getTerminator();
      auto *br = dyn_cast<BranchInst>(terminator);
      if (br) {
        if (br->isConditional()) {
          if (auto *cmp = dyn_cast<ICmpInst>(br->getCondition())) {
            // br: a op b == true or false
            // makeAllowedICmpRegion turning a op b into a range.
            auto *lhs = cmp->getOperand(0);
            auto *rhs = cmp->getOperand(1);

            if (!lhs->getType()->isIntegerTy() ||
                !rhs->getType()->isIntegerTy()) {
              // This should be covered by `ICmpInst`.
              MKINT_WARN() << "The br operands are not both integers: " << *cmp;
            } else {
              auto lrng = get_range_by_bb(lhs, pred, func2range_info,
                                          global2range),
                   rrng = get_range_by_bb(rhs, pred, func2range_info,
                                          global2range);

              bool is_true_br = br->getSuccessor(0) == bb;
              if (is_true_br) { // T branch
                crange lprng = crange::cmpRegion()(cmp->getPredicate(), rrng);
                crange rprng =
                    crange::cmpRegion()(cmp->getSwappedPredicate(), lrng);
                // intersectWith requires same bit width; align cmp-derived
                // ranges to operand widths.
                lprng = alignToBitWidth(lprng, lrng.getBitWidth());
                rprng = alignToBitWidth(rprng, rrng.getBitWidth());

                // Don't change constant's value.
                branch_rng[lhs] = dyn_cast<ConstantInt>(lhs)
                                      ? lrng
                                      : lrng.intersectWith(lprng);
                branch_rng[rhs] = dyn_cast<ConstantInt>(rhs)
                                      ? rrng
                                      : rrng.intersectWith(rprng);
              } else { // F branch
                crange lprng =
                    crange::cmpRegion()(cmp->getInversePredicate(), rrng);
                crange rprng = crange::cmpRegion()(
                    CmpInst::getInversePredicate(cmp->getPredicate()), lrng);
                lprng = alignToBitWidth(lprng, lrng.getBitWidth());
                rprng = alignToBitWidth(rprng, rrng.getBitWidth());

                // Don't change constant's value.
                branch_rng[lhs] = dyn_cast<ConstantInt>(lhs)
                                      ? lrng
                                      : lrng.intersectWith(lprng);
                branch_rng[rhs] = dyn_cast<ConstantInt>(rhs)
                                      ? rrng
                                      : rrng.intersectWith(rprng);
              }

              if (!RobustReachability && (branch_rng[lhs].isEmptySet() ||
                                          branch_rng[rhs].isEmptySet()))
                impossible_branches[cmp] =
                    is_true_br; // TODO: higher precision.
              else
                branch_rng[cmp] = crange(APInt(1, is_true_br));
            }
          }
        }
      } else if (auto *swt = dyn_cast<SwitchInst>(terminator)) {
        auto *cond = swt->getCondition();
        if (cond->getType()->isIntegerTy()) {
          auto cond_rng =
              get_range_by_bb(cond, pred, func2range_info, global2range);
          auto emp_rng =
              crange::getEmpty(cond->getType()->getIntegerBitWidth());

          if (swt->getDefaultDest() == bb) { // default
            // not (all)
            for (auto c : swt->cases()) {
              auto *case_val = c.getCaseValue();
              if (emp_rng.getBitWidth() == case_val->getValue().getBitWidth()) {
                emp_rng = emp_rng.unionWith(case_val->getValue());
              } else if (emp_rng.isEmptySet()) {
                emp_rng = crange(case_val->getValue());
              }
            }
            emp_rng = emp_rng.inverse();
          } else {
            for (auto c : swt->cases()) {
              if (c.getCaseSuccessor() == bb) {
                auto *case_val = c.getCaseValue();
                if (emp_rng.getBitWidth() ==
                    case_val->getValue().getBitWidth()) {
                  emp_rng = emp_rng.unionWith(case_val->getValue());
                } else if (emp_rng.isEmptySet()) {
                  emp_rng = crange(case_val->getValue());
                }
              }
            }
          }

          if (cond_rng.getBitWidth() == emp_rng.getBitWidth()) {
            branch_rng[cond] = cond_rng.intersectWith(emp_rng);
          } else {
            branch_rng[cond] = cond_rng;
          }
        }
      } else {
        // Invoke, IndirectBr, CallBr, etc. — proceed conservatively without
        // adding branch-specific range constraints.  Using MKINT_CHECK_RELAX
        // (not ABORT) so the analysis continues rather than crashing.
        MKINT_CHECK_RELAX(false) << "[Range Analysis] Unknown terminator "
                                    "(proceeding conservatively): "
                                 << *pred->getTerminator();
      }

      analyze_one_bb_range(bb, branch_rng, func2range_info, backedges,
                           global2range, garr2ranges, gep_oob,
                           impossible_branches, func2tsrc, callback_tsrc_fn,
                           func2ret_range, DL, AA, MSSA);
    }

    if (bb->isEntryBlock()) {
      MKINT_LOG() << "No predecessors: " << bb;
      analyze_one_bb_range(bb, sum_rng, func2range_info, backedges,
                           global2range, garr2ranges, gep_oob,
                           impossible_branches, func2tsrc, callback_tsrc_fn,
                           func2ret_range, DL, AA, MSSA);
    }
  }
}

std::string get_bb_label(const BasicBlock *bb) {
  // Check if func and module are available, avoiding 'Segmentfault'
  if (!bb || !bb->getParent() || bb->getParent()->getName().empty() ||
      !bb->getParent()->getParent())
    return "<badref>";
  std::string str;
  llvm::raw_string_ostream os(str);
  bb->printAsOperand(os, false);
  return str;
}

void RangeAnalysis::init_ranges(
    Module &M, std::map<const Function *, bbrange_t> &func2range_info,
    std::map<const Function *, crange> &func2ret_range,
    SetVector<Function *> &range_analysis_funcs,
    std::map<const GlobalVariable *, crange> &global2range,
    std::map<const GlobalVariable *, SmallVector<crange, 4>> &garr2ranges,
    const SetVector<Function *> &taint_funcs,
    const SetVector<StringRef> &callback_tsrc_fn) {
  for (auto &F : M) {
    // Functions for range analysis:
    // 1. taint source -> taint sink.
    // 2. integer functions.
    MKINT_LOG() << "Init Range Analysis: " << F.getName();
    if (F.getReturnType()->isIntegerTy() || taint_funcs.contains(&F)) {
      if (F.isDeclaration()) {
        if (TaintAnalysis::is_taint_src_arg_call(F.getName()) &&
            !taint_funcs.contains(&F) // will not call sink fns.
            &&
            callback_tsrc_fn.contains(F.getName().substr(
                0, F.getName().size() - StringRef(".mkint.arg").size() - 1))) {
          if (F.getReturnType()->isIntegerTy())
            func2ret_range[&F] =
                crange(F.getReturnType()->getIntegerBitWidth(), false);
          MKINT_LOG() << "Skip range analysis for func w/o impl [Empty Set]: "
                      << F.getName() << "\tin taint_funcs? ";
        } else {
          if (F.getReturnType()->isIntegerTy())
            func2ret_range[&F] =
                crange(F.getReturnType()->getIntegerBitWidth(), true); // full.
          MKINT_LOG() << "Skip range analysis for func w/o impl [Full Set]: "
                      << F.getName();
        }
      } else {
        if (F.getReturnType()->isIntegerTy())
          func2ret_range[&F] =
              crange(F.getReturnType()->getIntegerBitWidth(), false); // empty.

        // init the arg range
        auto &init_blk = func2range_info[&F][&(F.getEntryBlock())];
        for (const auto &arg : F.args()) {
          if (arg.getType()->isIntegerTy()) {
            // be conservative first.
            // TODO: fine-grained arg range (some taint, some not)
            if (TaintAnalysis::is_taint_src(F.getName()) &&
                !callback_tsrc_fn.contains(
                    F.getName())) { // for taint source, we assume full set.
              init_blk[&arg] =
                  crange(arg.getType()->getIntegerBitWidth(), true);
            } else {
              init_blk[&arg] =
                  crange(arg.getType()->getIntegerBitWidth(), false);
            }
          }
        }
        range_analysis_funcs.insert(&F);
      }
    }
  }

  // m_callback_tsrc_fn's highest user's input is set as full set.
  for (auto &fn : callback_tsrc_fn) {
    auto *cbf = M.getFunction(fn);

    std::deque<Function *> worklist;
    SetVector<Function *> hist;

    worklist.push_back(cbf);
    hist.insert(cbf);

    while (!worklist.empty()) {
      auto *cur = worklist.front();
      worklist.pop_front();

      if (cur->user_empty()) {
        for (const auto &arg : cur->args()) {
          func2range_info[cur][&(cur->getEntryBlock())][&arg] =
              crange(arg.getType()->getIntegerBitWidth(), true);
        }
      } else {
        for (const auto &u : cur->users()) {
          if (auto *uu = dyn_cast<CallBase>(u)) {
            auto *caller = uu->getFunction();
            if (caller && !hist.contains(caller)) {
              worklist.push_back(caller);
              hist.insert(caller);
            }
          }
        }
      }
    }
  }

  // global variables
  for (const auto &GV : M.globals()) {
    MKINT_LOG() << "Found global var " << GV.getName() << " of type "
                << *GV.getType();
    if (GV.getValueType()->isIntegerTy()) {
      if (GV.hasInitializer()) {
        auto init_val = dyn_cast<ConstantInt>(GV.getInitializer())->getValue();
        MKINT_LOG() << GV.getName() << " init by " << init_val;
        global2range[&GV] = crange(init_val);
      } else {
        global2range[&GV] = crange(
            GV.getValueType()->getIntegerBitWidth()); // can be all range.
      }
    } else if (GV.getValueType()->isArrayTy()) { // int array.
      auto *const garr = dyn_cast<ArrayType>(GV.getValueType());
      if (garr->getElementType()->isIntegerTy()) {
        if (GV.hasInitializer()) {
          if (const auto *darr =
                  dyn_cast<ConstantDataArray>(GV.getInitializer())) {
            for (size_t i = 0; i < darr->getNumElements(); i++) {
              auto init_val =
                  dyn_cast<ConstantInt>(darr->getElementAsConstant(i))
                      ->getValue();
              MKINT_LOG() << GV.getName() << "[" << i << "] init by "
                          << init_val;
              garr2ranges[&GV].push_back(crange(init_val));
            }
          } else if (const auto *zinit =
                         dyn_cast<ConstantAggregateZero>(GV.getInitializer())) {
            for (size_t i = 0; i < zinit->getElementCount().getFixedValue();
                 i++) {
              auto *elemType = zinit->getElementValue(i)->getType();
              if (elemType->isIntegerTy()) {
                garr2ranges[&GV].push_back(crange(
                    APInt::getNullValue(elemType->getIntegerBitWidth())));
              } else {
                MKINT_WARN()
                    << "Skipping non-integer element type in global array: "
                    << GV.getName();
              }
            }
          } else {
            MKINT_CHECK_ABORT(false)
                << "Unsupported initializer for global array: " << GV.getName();
          }
        } else {
          for (size_t i = 0; i < garr->getNumElements(); i++) {
            garr2ranges[&GV].push_back( // can be anything
                crange(garr->getElementType()->getIntegerBitWidth(), true));
          }
        }
      } else {
        MKINT_WARN() << "Unhandled global var type: " << *GV.getType() << " -> "
                     << GV.getName();
      }
    } else {
      MKINT_WARN() << "Unhandled global var type: " << *GV.getType() << " -> "
                   << GV.getName();
    }
  }
}

void RangeAnalysis::print_all_ranges(
    const std::map<const Function *, crange> &func2ret_range,
    const std::map<const GlobalVariable *, crange> &global2range,
    const std::map<const GlobalVariable *, SmallVector<crange, 4>> &garr2ranges,
    const std::map<const Function *, bbrange_t> &func2range_info,
    const std::map<ICmpInst *, bool> &impossible_branches,
    const std::set<GetElementPtrInst *> &gep_oob) const {
  MKINT_LOG() << "========== Function Return Ranges ==========";
  for (const auto &func_rng_pair : func2ret_range) {
    const auto *F = func_rng_pair.first;
    auto rng = func_rng_pair.second;
    MKINT_LOG() << rang::bg::black << rang::fg::green << F->getName()
                << rang::style::reset << " -> " << rng;
  }

  MKINT_LOG() << "========== Global Variable Ranges ==========";
  for (const auto &global_rng_pair : global2range) {
    const auto *GV = global_rng_pair.first;
    auto rng = global_rng_pair.second;
    MKINT_LOG() << rang::bg::black << rang::fg::blue << GV->getName()
                << rang::style::reset << " -> " << rng;
  }

  for (const auto &global_rngvec_pair : garr2ranges) {
    const auto *GV = global_rngvec_pair.first;
    auto rng_vec = global_rngvec_pair.second;
    for (size_t i = 0; i < rng_vec.size(); i++) {
      MKINT_LOG() << rang::bg::black << rang::fg::blue << GV->getName() << "["
                  << i << "]" << rang::style::reset << " -> " << rng_vec[i];
    }
  }

  MKINT_LOG() << "============ Function Inst Ranges ============";
  for (const auto &func_blk2rng_pair : func2range_info) {
    const auto *F = func_blk2rng_pair.first;
    auto &blk2rng = func_blk2rng_pair.second;
    MKINT_LOG() << " ----------- Function Name : " << rang::bg::black
                << rang::fg::green << F->getName() << rang::style::reset;
    for (const auto &blk_inst2rng_pair : blk2rng) {
      const auto *blk = blk_inst2rng_pair.first;
      auto &inst2rng = blk_inst2rng_pair.second;
      MKINT_LOG() << " ----------- Basic Block " << get_bb_label(blk)
                  << " ----------- ";
      for (const auto &val_rng_pair : inst2rng) {
        const auto *val = val_rng_pair.first;
        auto rng = val_rng_pair.second;
        if (isa<ConstantInt>(val))
          continue; // meaningless to pring const range.

        if (rng.isFullSet())
          MKINT_LOG() << *val << "\t -> " << rng;
        else
          MKINT_LOG() << *val << "\t -> " << rang::bg::black << rang::fg::yellow
                      << rng << rang::style::reset;
      }
    }
  }

  if (!impossible_branches.empty()) {
    MKINT_WARN() << "============" << rang::fg::yellow << rang::style::bold
                 << " Impossible Branches " << rang::style::reset
                 << "============";
    for (auto &cmp_istbr_pair : impossible_branches) {
      auto *cmp = cmp_istbr_pair.first;
      auto is_tbr = cmp_istbr_pair.second;
      MKINT_WARN() << rang::bg::black << rang::fg::red
                   << cmp->getFunction()->getName() << "::" << *cmp
                   << rang::style::reset << "'s " << rang::fg::red
                   << rang::style::italic << (is_tbr ? "true" : "false")
                   << rang::style::reset << " branch";
    }
  }

  if (!gep_oob.empty()) {
    MKINT_WARN() << "============" << rang::fg::yellow << rang::style::bold
                 << " Array Index Out of Bound " << rang::style::reset
                 << "============";
    for (auto *gep : gep_oob) {
      MKINT_WARN() << rang::bg::black << rang::fg::red
                   << gep->getFunction()->getName() << "::" << *gep
                   << rang::style::reset;
    }
  }
}

} // namespace kint
