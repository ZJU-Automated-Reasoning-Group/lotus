#ifndef LOTUS_DATAFLOW_MONO_SOLVER_INTRASOLVER_H_
#define LOTUS_DATAFLOW_MONO_SOLVER_INTRASOLVER_H_

#include "llvm/IR/CFG.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/ControlFlow/IntraCFG.h"
#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Support/MonoDebug.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mono {

template <typename AnalysisTypesT> class IntraMonoSolver {
public:
  using ProblemTy = IntraMonoProblem<AnalysisTypesT>;
  using n_t = typename AnalysisTypesT::n_t;
  using mono_container_t = typename AnalysisTypesT::mono_container_t;
  using CFGTy = typename AnalysisTypesT::c_t;

  explicit IntraMonoSolver(ProblemTy &Problem)
      : Problem(Problem), CFG(selectCFG()) {
    MissingResultFallback = this->Problem.bottom();
  }

  void setDebugConfig(const DebugConfig &Config) { DebugCfg = Config; }

  const DebugConfig &getDebugConfig() const { return DebugCfg; }

  const SolverStatistics &getStatistics() const { return Stats; }

  /**
   * @brief Set the widening threshold (L1 fix)
   *
   * When a node has been re-processed more than this many times, the solver
   * calls Problem.widen(OldOut, NewOut) instead of using NewOut directly.
   * This guarantees termination for analyses over infinite-height lattices
   * (e.g., interval analysis) provided the widen() implementation is correct.
   *
   * Set to 0 to disable widening entirely (default behaviour for analyses
   * over finite-height lattices that converge without widening).
   *
   * Typical values: 1–5.  Lower values converge faster but may be less
   * precise; higher values are more precise but may take longer to converge.
   *
   * @param Threshold Number of re-processings before widening kicks in
   */
  void setWideningThreshold(unsigned Threshold) {
    WideningThreshold = Threshold;
  }

  unsigned getWideningThreshold() const { return WideningThreshold; }

  void solve() {
    resetStateForSolve();
    auto start_time = std::chrono::steady_clock::now();

    MONO_TRACE_WORKLIST(llvm::outs(), DebugCfg,
                        "Starting IntraMonoSolver::solve()");

    initialize();

    auto init_end_time = std::chrono::steady_clock::now();
    Stats.initialization_time =
        std::chrono::duration_cast<std::chrono::microseconds>(init_end_time -
                                                              start_time);

    auto solve_start_time = std::chrono::steady_clock::now();

    // Standard monotone node-based worklist algorithm (fixes #1 and #2).
    //
    // We maintain:
    //   AnalysisIn[n]  = IN[n]  = join(OUT[p] for all preds p of n)
    //   AnalysisOut[n] = OUT[n] = normalFlow(n, IN[n])
    //
    // The worklist contains nodes (not edges).  When we dequeue node n:
    //   1. Recompute IN[n] by merging OUT[p] for every predecessor p.
    //      Predecessors with no OUT yet are skipped (treated as identity for
    //      the join, i.e., they contribute nothing until they are computed).
    //   2. Recompute OUT[n] = normalFlow(n, IN[n]).
    //   3. If OUT[n] changed, enqueue all successors of n.
    //
    // This correctly handles join points: every predecessor's latest OUT
    // contributes to IN[n], not just the one that triggered the re-evaluation.
    //
    // Seeds: initialSeeds() pre-populates AnalysisIn for boundary nodes.
    // Those values are used as the starting IN for those nodes; if a seed
    // node has predecessors whose OUT later becomes non-empty, the seed value
    // is merged with (not replaced by) the predecessor contributions.

    // Convert the edge-based initial worklist to a node-based one.
    // We use a deque + in-queue set to avoid duplicate entries.
    std::deque<n_t> NodeWorklist;
    std::unordered_set<n_t> InNodeQueue;

    auto EnqueueNode = [&](n_t Node) {
      if (Node != nullptr && InNodeQueue.insert(Node).second) {
        NodeWorklist.push_back(Node);
      }
    };

    // Seed the node worklist from the edge worklist built by initialize().
    // We enqueue both endpoints of every edge and every discovered node so
    // each instruction gets an initial visit even for sparse/empty edge sets.
    for (const auto &Edge : Worklist) {
      EnqueueNode(Edge.first);
      EnqueueNode(Edge.second);
    }
    for (auto *Node : InitialNodes) {
      EnqueueNode(Node);
    }
    Worklist.clear(); // edge worklist no longer needed

    while (!NodeWorklist.empty()) {
      Stats.record_worklist_size(NodeWorklist.size());
      Stats.iterations++;

      if (DebugCfg.is_enabled(DebugLevel::Debug) &&
          Stats.iterations <= DebugCfg.max_iterations_log) {
        llvm::outs() << "[WORKLIST] Iteration " << Stats.iterations
                     << ", size=" << NodeWorklist.size() << "\n";
      }

      n_t Node = NodeWorklist.front();
      NodeWorklist.pop_front();
      InNodeQueue.erase(Node);
      Stats.worklist_total_pops++;
      Stats.nodes_processed++;

      MONO_TRACE_WORKLIST(llvm::outs(), DebugCfg, "Processing node: " << Node);

      // Step 1: recompute IN[Node] = join(OUT[p] for all preds p).
      // Predecessors not yet in AnalysisOut are skipped; they will trigger
      // re-evaluation of Node when they are processed later.
      auto Preds = CFG->getPredsOf(Node, Problem.direction());
      mono_container_t NewIn = Problem.bottom();
      bool HasContrib = false;

      // Preserve explicit seed facts as boundary conditions on every
      // recomputation: IN[n] = seed[n] join join(OUT[p] for preds p of n).
      // This matches the interprocedural engine's SeedIns semantics and makes
      // mid-function/source seeds persistent rather than one-shot
      // initializations.
      auto SeedIt = SeedFacts.find(Node);
      if (SeedIt != SeedFacts.end()) {
        NewIn = SeedIt->second;
        HasContrib = true;
      }

      for (auto *Pred : Preds) {
        auto OutIt = AnalysisOut.find(Pred);
        if (OutIt == AnalysisOut.end()) {
          continue; // predecessor not yet computed
        }
        Stats.merge_operations++;
        if (!HasContrib) {
          NewIn = OutIt->second;
          HasContrib = true;
        } else {
          NewIn = Problem.join(NewIn, OutIt->second);
        }
      }

      if (!HasContrib) {
        // No seed and no predecessor has a computed OUT yet. Use the existing
        // AnalysisIn value (which may be bottom()) rather than overwriting it.
        auto InIt = AnalysisIn.find(Node);
        if (InIt != AnalysisIn.end()) {
          NewIn = InIt->second;
        }
        // else: NewIn stays as bottom() (the default initial value)
      }

      // Step 2: check whether IN[Node] changed.
      Stats.stabilization_checks++;
      auto InIt = AnalysisIn.find(Node);
      bool InChanged =
          (InIt == AnalysisIn.end()) || !Problem.equal(NewIn, InIt->second);

      if (InChanged) {
        AnalysisIn[Node] = NewIn;
      }

      // Step 3: recompute OUT[Node] = normalFlow(Node, IN[Node]).
      Stats.flow_function_calls++;
      auto NewOut = Problem.normalFlow(Node, AnalysisIn[Node]);

      if (WideningThreshold > 0u) {
        unsigned &Count = NodeIterCount[Node];
        ++Count;
        if (Count > WideningThreshold) {
          auto OutWideIt = AnalysisOut.find(Node);
          if (OutWideIt != AnalysisOut.end()) {
            NewOut = Problem.widen(OutWideIt->second, NewOut);
          }
        }
      }

      auto OutIt = AnalysisOut.find(Node);
      bool OutChanged = (OutIt == AnalysisOut.end()) ||
                        !Problem.equal(NewOut, OutIt->second);

      if (OutChanged) {
        MONO_TRACE_FACTS(llvm::outs(), DebugCfg, "Facts changed at " << Node);
        AnalysisOut[Node] = std::move(NewOut);

        // Enqueue all successors so they recompute their IN.
        for (auto *Succ : CFG->getSuccsOf(Node, Problem.direction())) {
          EnqueueNode(Succ);
        }
      }
    }

    // AnalysisIn and AnalysisOut are both fully populated above.
    // No extra normalFlow pass needed (that was bug #2).

    auto solve_end_time = std::chrono::steady_clock::now();
    Stats.solving_time = std::chrono::duration_cast<std::chrono::microseconds>(
        solve_end_time - solve_start_time);
    Stats.total_time = std::chrono::duration_cast<std::chrono::microseconds>(
        solve_end_time - start_time);

    MONO_TRACE_WORKLIST(llvm::outs(), DebugCfg,
                        "IntraMonoSolver finished after " << Stats.iterations
                                                          << " iterations");

    if (DebugCfg.collect_statistics) {
      Stats.dump(llvm::outs());
    }
  }

  const mono_container_t &getResultsAt(n_t Stmt) const {
    return getInResultsAt(Stmt);
  }

  const mono_container_t &getInResultsAt(n_t Stmt) const {
    auto It = AnalysisIn.find(Stmt);
    if (It != AnalysisIn.end()) {
      return It->second;
    }
    return MissingResultFallback;
  }

  const mono_container_t &getOutResultsAt(n_t Stmt) const {
    auto It = AnalysisOut.find(Stmt);
    if (It != AnalysisOut.end()) {
      return It->second;
    }
    return MissingResultFallback;
  }

  const std::unordered_map<n_t, mono_container_t> &getInResults() const {
    return AnalysisIn;
  }

  const std::unordered_map<n_t, mono_container_t> &getOutResults() const {
    return AnalysisOut;
  }

  void dumpResults(llvm::raw_ostream &OS = llvm::outs()) const {
    OS << "\n================ IntraMonoSolver results ================\n";
    if (AnalysisIn.empty()) {
      OS << "No results computed!\n";
      return;
    }
    std::vector<std::pair<n_t, mono_container_t>> Cells;
    Cells.reserve(AnalysisIn.size());
    Cells.insert(Cells.end(), AnalysisIn.begin(), AnalysisIn.end());
    std::sort(Cells.begin(), Cells.end(), [](const auto &Lhs, const auto &Rhs) {
      return Lhs.first < Rhs.first;
    });
    for (const auto &Cell : Cells) {
      n_t Node = Cell.first;
      const auto &Facts = Cell.second;
      OS << "Instruction: ";
      if (Node != nullptr) {
        OS << *Node;
      } else {
        OS << "<null>";
      }
      OS << "\nFacts: ";
      if (Facts.empty()) {
        OS << "EMPTY\n";
      } else {
        Problem.printContainer(OS, Facts);
        OS << "\n";
      }
    }
  }

  void emitTextReport(llvm::raw_ostream & /*OS*/ = llvm::outs()) const {}
  void emitGraphicalReport(llvm::raw_ostream & /*OS*/ = llvm::outs()) const {}

private:
  const ::dataflow::controlflow::IntraCFG *selectCFG() {
    if (auto *Provided = Problem.getCFG()) {
      return Provided;
    }
    // Use a per-instance CFG instead of a static singleton.
    // The static singleton was shared across all IntraMonoSolver instances,
    // causing data races when multiple solvers ran concurrently and stale
    // state when the same solver was reused across different functions.
    return &PerInstanceCFG;
  }

  void resetStateForSolve() {
    Worklist.clear();
    InitialNodes.clear();
    AnalysisIn.clear();
    AnalysisOut.clear();
    SeedFacts.clear();
    NodeIterCount.clear();
    MissingResultFallback = Problem.bottom();
    Stats = SolverStatistics{};
  }

  void initialize() {
    std::vector<llvm::Function *> EntryFunctions = Problem.getEntryPoints();

    std::unordered_set<llvm::Function *> SeenFunctions;
    for (auto *Function : EntryFunctions) {
      if (Function == nullptr || Function->isDeclaration()) {
        continue;
      }
      SeenFunctions.insert(Function);
      auto Edges = CFG->getAllControlFlowEdges(Function, Problem.direction());
      Worklist.insert(Worklist.begin(), Edges.begin(), Edges.end());
      for (auto *Inst : CFG->getAllInstructionsOf(Function)) {
        AnalysisIn.insert({Inst, Problem.bottom()});
        InitialNodes.push_back(Inst);
      }
    }

    // Ensure any function that contains a seed node has its CFG in the
    // worklist so propagation from that seed can occur (correctness when
    // initialSeeds() targets instructions outside EntryPoints).
    auto Seeds = Problem.initialSeeds();
    SeedFacts = Seeds;
    for (const auto &Entry : Seeds) {
      auto *BB = Entry.first ? Entry.first->getParent() : nullptr;
      auto *F = BB ? BB->getParent() : nullptr;
      if (F && !F->isDeclaration() && SeenFunctions.insert(F).second) {
        auto Edges = CFG->getAllControlFlowEdges(F, Problem.direction());
        Worklist.insert(Worklist.begin(), Edges.begin(), Edges.end());
        for (auto *Inst : CFG->getAllInstructionsOf(F)) {
          AnalysisIn.insert({Inst, Problem.bottom()});
          InitialNodes.push_back(Inst);
        }
      }
    }

    // Apply explicit seed facts as boundary conditions. Seeds are allowed at
    // arbitrary program points; they persist across recomputation via
    // SeedFacts in the solve() loop.
    for (const auto &Entry : Seeds) {
      auto *SeedInst = Entry.first;
      if (SeedInst == nullptr) {
        continue;
      }
      auto *BB = SeedInst->getParent();
      auto *F = BB ? BB->getParent() : nullptr;
      if (F == nullptr || F->isDeclaration()) {
        continue;
      }
      AnalysisIn[SeedInst] = Entry.second;
    }
  }

  ProblemTy &Problem;
  /// Per-instance CFG (replaces the static singleton).
  ::dataflow::controlflow::LLVMIntraCFG PerInstanceCFG;
  const ::dataflow::controlflow::IntraCFG *CFG;
  std::deque<std::pair<n_t, n_t>> Worklist;
  std::vector<n_t> InitialNodes;
  std::unordered_map<n_t, mono_container_t> AnalysisIn;
  std::unordered_map<n_t, mono_container_t> AnalysisOut;
  std::unordered_map<n_t, mono_container_t> SeedFacts;
  mono_container_t MissingResultFallback{};
  DebugConfig DebugCfg;
  SolverStatistics Stats;
  std::unordered_map<n_t, unsigned> NodeIterCount;
  unsigned WideningThreshold = 0u;
};

template <typename Problem>
using IntraMonoSolver_P =
    IntraMonoSolver<typename Problem::ProblemAnalysisTypes>;

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_SOLVER_INTRASOLVER_H_
