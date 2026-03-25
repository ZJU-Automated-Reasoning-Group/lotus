#include "llvm/Analysis/AssumeBundleQueries.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

#include "Dataflow/APA/Adapters/LLVM/ForwardProblem.h"
#include "Dataflow/APA/Clients/LLVM/Intra/NonNull.h"
#include "Dataflow/APA/Core/Problem.h"
#include "Dataflow/APA/Engines/Solver.h"
#include "Dataflow/ControlFlow/IntraCFG.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace elimination {
namespace {

struct NonNullDomain {
  using n_t = llvm::Instruction *;
  using fact_t = NonNullFact;
  using transfer_t = NonNullEdgeTransfer;
};

class NonNullProblem : public IntraReducibleEliminationProblem<NonNullDomain> {
public:
  explicit NonNullProblem(llvm::Function *F, llvm::AssumptionCache *AC,
                          llvm::DominatorTree *DT)
      : F(F), AC(AC), DT(DT),
        DL(F != nullptr ? &F->getParent()->getDataLayout() : nullptr) {
    buildUniverse();
  }

  std::vector<n_t> nodes() const override {
    ensurePrepared();
    return Nodes;
  }

  n_t entry() const override {
    if (F == nullptr || F->isDeclaration()) {
      return nullptr;
    }
    return &*F->getEntryBlock().begin();
  }

  std::vector<n_t> succs(n_t Node) const override {
    return CFG.getSuccsOf(Node, dataflow::controlflow::FlowDirection::Forward);
  }

  transfer_t edgeTransfer(n_t Src, n_t Dst) const override {
    return NonNullEdgeTransfer{Src, Dst};
  }

  fact_t applyTransfer(const transfer_t &T, const fact_t &In) const override {
    fact_t Out = In;
    auto *Src = T.Src;
    auto *Dst = T.Dst;
    if (Src == nullptr) {
      return Out;
    }

    addNonNullFromInstruction(Src, In, Out);
    addNonNullFromAssume(Src, Out);
    addNonNullFromAssumeBundles(Src, Out);
    addNonNullFromBranch(Src, Dst, Out);

    return Out;
  }

  fact_t meet(const fact_t &Lhs, const fact_t &Rhs) const override {
    fact_t Out;
    std::set_intersection(Lhs.begin(), Lhs.end(), Rhs.begin(), Rhs.end(),
                          std::inserter(Out, Out.begin()));
    return Out;
  }

  bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const override {
    return Lhs == Rhs;
  }

  fact_t meetIdentity() const override { return Universe; }

  fact_t initialFact() const override {
    fact_t Out;
    if (F == nullptr || F->isDeclaration()) {
      return Out;
    }
    for (auto &Arg : F->args()) {
      if (Arg.getType()->isPointerTy() && Arg.hasNonNullAttr()) {
        Out.insert(&Arg);
      }
    }
    return Out;
  }

  // --- IntraReducibleEliminationProblem interface ---

  std::vector<Edge> edges() const override {
    ensurePrepared();
    std::vector<Edge> Result;
    for (auto *Src : Nodes) {
      for (auto *Dst :
           CFG.getSuccsOf(Src, dataflow::controlflow::FlowDirection::Forward)) {
        Result.push_back({Src, Dst});
      }
    }
    return Result;
  }

  std::vector<n_t> topologicalOrder() const override {
    ensurePrepared();
    if (Nodes.empty()) {
      return {};
    }
    // Build forward-edge adjacency and in-degree, skipping back edges.
    std::unordered_map<n_t, std::vector<n_t>> Succ;
    std::unordered_map<n_t, std::size_t> InDeg;
    for (auto *N : Nodes) {
      InDeg[N] = 0;
    }
    for (auto *Src : Nodes) {
      for (auto *Dst :
           CFG.getSuccsOf(Src, dataflow::controlflow::FlowDirection::Forward)) {
        if (!dominates(Dst, Src)) { // skip back edges
          Succ[Src].push_back(Dst);
          ++InDeg[Dst];
        }
      }
    }
    auto *EntryNode = entry();
    std::deque<n_t> Ready;
    Ready.push_back(EntryNode);
    for (auto *N : Nodes) {
      if (N != EntryNode && InDeg[N] == 0) {
        Ready.push_back(N);
      }
    }
    std::vector<n_t> Topo;
    Topo.reserve(Nodes.size());
    while (!Ready.empty()) {
      auto *Cur = Ready.front();
      Ready.pop_front();
      Topo.push_back(Cur);
      for (auto *S : Succ[Cur]) {
        if (--InDeg[S] == 0) {
          Ready.push_back(S);
        }
      }
    }
    if (Topo.size() != Nodes.size()) {
      return {}; // cycle detected (irreducible)
    }
    return Topo;
  }

  n_t idom(n_t Node) const override {
    if (Node == nullptr || F == nullptr || F->isDeclaration()) {
      return Node;
    }
    if (Node == entry()) {
      return entry();
    }
    const auto *BB = Node->getParent();
    // Within a block: idom of a non-first instruction is its predecessor.
    if (&BB->front() != Node) {
      return Node->getPrevNode();
    }
    // First instruction of a block: idom is the last instruction of the
    // immediate dominator block.
    auto *EffDT = getDT();
    const auto *DTNode = EffDT->getNode(const_cast<llvm::BasicBlock *>(BB));
    if (DTNode == nullptr || DTNode->getIDom() == nullptr) {
      return entry();
    }
    const auto *IDomBB = DTNode->getIDom()->getBlock();
    if (IDomBB == nullptr) {
      return entry();
    }
    return const_cast<n_t>(IDomBB->getTerminator());
  }

  bool dominates(n_t A, n_t B) const override {
    if (A == nullptr || B == nullptr) {
      return false;
    }
    if (A == B) {
      return true;
    }
    const auto *BBA = A->getParent();
    const auto *BBB = B->getParent();
    if (BBA == BBB) {
      return A->comesBefore(B);
    }
    return getDT()->dominates(BBA, BBB);
  }

private:
  // Return the effective dominator tree: prefer the externally provided one,
  // otherwise lazily compute and cache our own.
  llvm::DominatorTree *getDT() const {
    if (DT != nullptr) {
      return DT;
    }
    if (!OwnedDT) {
      OwnedDT = std::make_unique<llvm::DominatorTree>(*F);
    }
    return OwnedDT.get();
  }

  mutable std::unique_ptr<llvm::DominatorTree> OwnedDT;

private:
  llvm::Function *F = nullptr;
  llvm::AssumptionCache *AC = nullptr;
  llvm::DominatorTree *DT = nullptr;
  const llvm::DataLayout *DL = nullptr;
  dataflow::controlflow::LLVMIntraCFG CFG;
  mutable bool Prepared = false;
  mutable std::vector<n_t> Nodes;
  fact_t Universe;

  void buildUniverse() {
    Universe.clear();
    if (F == nullptr || F->isDeclaration()) {
      return;
    }
    for (auto &Arg : F->args()) {
      if (Arg.getType()->isPointerTy()) {
        Universe.insert(&Arg);
      }
    }
    for (auto &BB : *F) {
      for (auto &I : BB) {
        if (I.getType()->isPointerTy()) {
          Universe.insert(&I);
        }
      }
    }
  }

  void ensurePrepared() const {
    if (Prepared) {
      return;
    }
    Prepared = true;
    Nodes.clear();
    if (F == nullptr || F->isDeclaration()) {
      return;
    }

    auto *Entry = entry();
    if (Entry == nullptr) {
      return;
    }

    std::unordered_set<n_t> Reach;
    std::vector<n_t> Stack;
    Stack.push_back(Entry);
    Reach.insert(Entry);
    while (!Stack.empty()) {
      auto *Cur = Stack.back();
      Stack.pop_back();
      for (auto *Succ :
           CFG.getSuccsOf(Cur, dataflow::controlflow::FlowDirection::Forward)) {
        if (Reach.insert(Succ).second) {
          Stack.push_back(Succ);
        }
      }
    }

    for (auto *N : CFG.getAllInstructionsOf(F)) {
      if (Reach.count(N)) {
        Nodes.push_back(N);
      }
    }
  }

  bool isKnownNonNullValue(const llvm::Value *V,
                           const llvm::Instruction *Ctx) const {
    if (V == nullptr || !V->getType()->isPointerTy() || DL == nullptr) {
      return false;
    }
    return llvm::isKnownNonZero(V, *DL, 0, AC, Ctx, DT);
  }

  void addNonNullFromInstruction(llvm::Instruction *Inst, const fact_t &In,
                                 fact_t &Out) const {
    if (Inst == nullptr) {
      return;
    }

    if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(Inst)) {
      Out.insert(Alloca);
    }

    if (Inst->getType()->isPointerTy()) {
      if (isKnownNonNullValue(Inst, Inst)) {
        Out.insert(Inst);
      }

      if (auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
        if (Call->hasRetAttr(llvm::Attribute::NonNull)) {
          Out.insert(Call);
        }
        addNonNullFromCallAttrs(Call, Out);
      }

      if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(Inst)) {
        auto *Op = Cast->getOperand(0);
        if (In.count(Op)) {
          Out.insert(Cast);
        }
      }

      if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(Inst)) {
        auto *Base = GEP->getPointerOperand();
        if (In.count(Base)) {
          Out.insert(GEP);
        }
      }

      if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(Inst)) {
        auto *TVal = Select->getTrueValue();
        auto *FVal = Select->getFalseValue();
        if (In.count(TVal) && In.count(FVal)) {
          Out.insert(Select);
        }
      }

      if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(Inst)) {
        bool AllNonNull = true;
        for (auto &Incoming : Phi->incoming_values()) {
          if (!In.count(Incoming.get())) {
            AllNonNull = false;
            break;
          }
        }
        if (AllNonNull && Phi->getNumIncomingValues() > 0) {
          Out.insert(Phi);
        }
      }
    }

    addNonNullFromKnowledge(Inst, Out);
    for (auto &Op : Inst->operands()) {
      auto *V = Op.get();
      if (V != nullptr && V->getType()->isPointerTy()) {
        addNonNullFromKnowledge(V, Out, Inst);
      }
    }
  }

  static const llvm::Value *getNullCheckedPointer(const llvm::ICmpInst *Cmp) {
    if (Cmp == nullptr || !Cmp->isEquality()) {
      return nullptr;
    }
    auto *Op0 = Cmp->getOperand(0);
    auto *Op1 = Cmp->getOperand(1);
    if (llvm::isa<llvm::ConstantPointerNull>(Op0)) {
      return Op1;
    }
    if (llvm::isa<llvm::ConstantPointerNull>(Op1)) {
      return Op0;
    }
    return nullptr;
  }

  void addNonNullFromBranch(llvm::Instruction *Src, llvm::Instruction *Dst,
                            fact_t &Out) const {
    auto *BI = llvm::dyn_cast<llvm::BranchInst>(Src);
    if (BI == nullptr || !BI->isConditional()) {
      return;
    }
    auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(BI->getCondition());
    if (Cmp == nullptr) {
      return;
    }
    auto *Ptr = getNullCheckedPointer(Cmp);
    if (Ptr == nullptr) {
      return;
    }

    bool IsTrueSucc = BI->getSuccessor(0) == (Dst ? Dst->getParent() : nullptr);
    bool NonNullOnEdge = false;
    if (Cmp->getPredicate() == llvm::ICmpInst::ICMP_NE) {
      NonNullOnEdge = IsTrueSucc;
    } else if (Cmp->getPredicate() == llvm::ICmpInst::ICMP_EQ) {
      NonNullOnEdge = !IsTrueSucc;
    }

    if (NonNullOnEdge) {
      Out.insert(Ptr);
    }
  }

  void addNonNullFromAssume(llvm::Instruction *Src, fact_t &Out) const {
    auto *Call = llvm::dyn_cast<llvm::CallBase>(Src);
    if (Call == nullptr) {
      return;
    }
    if (Call->getIntrinsicID() != llvm::Intrinsic::assume) {
      return;
    }
    if (Call->arg_size() < 1) {
      return;
    }
    auto *Cond = Call->getArgOperand(0);
    auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(Cond);
    if (Cmp == nullptr) {
      return;
    }
    auto *Ptr = getNullCheckedPointer(Cmp);
    if (Ptr == nullptr) {
      return;
    }
    if (Cmp->getPredicate() == llvm::ICmpInst::ICMP_NE) {
      Out.insert(Ptr);
    }
  }

  void addNonNullFromAssumeBundles(llvm::Instruction *Src, fact_t &Out) const {
    // This helper intentionally delegates to ValueTracking knowledge at Src.
    // It does not enumerate assume operand bundles directly.
    if (AC == nullptr || Src == nullptr) {
      return;
    }
    if (Src->getType()->isPointerTy()) {
      addNonNullFromKnowledge(Src, Out);
    }
  }

  void addNonNullFromKnowledge(const llvm::Value *V, fact_t &Out,
                               const llvm::Instruction *Ctx = nullptr) const {
    if (V == nullptr || !V->getType()->isPointerTy()) {
      return;
    }
    if (AC == nullptr) {
      return;
    }
    auto K = llvm::getKnowledgeValidInContext(
        V, {llvm::Attribute::NonNull, llvm::Attribute::Dereferenceable},
        Ctx ? Ctx : llvm::dyn_cast_or_null<llvm::Instruction>(V), DT, AC);
    if (K && (K.AttrKind == llvm::Attribute::NonNull ||
              K.AttrKind == llvm::Attribute::Dereferenceable)) {
      Out.insert(V);
    }
  }

  void addNonNullFromCallAttrs(const llvm::CallBase *Call, fact_t &Out) const {
    if (Call == nullptr) {
      return;
    }
    auto *Callee = Call->getCalledFunction();
    if (Callee == nullptr) {
      return;
    }
    unsigned Idx = 0;
    for (auto &Arg : Call->args()) {
      if (!Arg->getType()->isPointerTy()) {
        ++Idx;
        continue;
      }
      if (Callee->hasParamAttribute(Idx, llvm::Attribute::NonNull) ||
          Callee->hasParamAttribute(Idx, llvm::Attribute::Dereferenceable)) {
        Out.insert(Arg.get());
      }
      ++Idx;
    }
  }
};

} // namespace

NonNullResult runIntraElimNonNull(llvm::Function *F, EliminationOptions Opts) {
  return runIntraElimNonNull(F, nullptr, nullptr, Opts);
}

NonNullResult runIntraElimNonNull(llvm::Function *F, llvm::AssumptionCache *AC,
                                  llvm::DominatorTree *DT,
                                  EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return NonNullResult{};
  }

  NonNullProblem Problem(F, AC, DT);
  IntraEliminationSolver<NonNullDomain> Solver(Problem, Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  Out.setSolveMetadata(Status, Solver.getDiagnostics());
  return Out;
}

} // namespace elimination
