/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/LLVM/BitVectorSolver.h"

#include "Dataflow/ControlFlow/IntraCFG.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace npa {

// Helper to generate unique symbols for blocks
static std::string getBlockSymbol(const llvm::BasicBlock *BB,
                                  const char *suffix) {
  std::string s;
  size_t suffixLen = strlen(suffix);
  s.reserve(sizeof(BB) + suffixLen + 1);
  s.append(reinterpret_cast<const char *>(&BB), sizeof(BB));
  s.push_back('_');
  s.append(suffix, suffixLen);
  return s;
}

BitVectorSolver::Result BitVectorSolver::run(llvm::Function &F,
                                             const BitVectorProblem &info,
                                             SolverStrategy strategy,
                                             LinearStrategy linearStrategy,
                                             bool verbose) {
  // 1. Setup Domain
  BitSetDomain::WidthScope width_scope(info.getBitWidth());

  bool forward = info.isForward();
  llvm::APInt boundary = info.getBoundaryVal();

  using D = BitSetDomain;
  using Exp = Exp0<D>;
  using E = E0<D>; // shared_ptr<Exp0<D>>
  ::dataflow::controlflow::LLVMIntraCFG CFG;

  std::vector<std::pair<Symbol, E>> eqns;

  // 2. Build Equations
  for (auto &BB : F) {
    // Symbols
    std::string inSym = getBlockSymbol(&BB, "IN");
    std::string outSym = getBlockSymbol(&BB, "OUT");

    llvm::APInt gen = info.getGen(&BB);
    llvm::APInt kill = info.getKill(&BB);
    llvm::APInt notKill = ~kill;

    if (forward) {
      // Forward Analysis
      // IN[B] = U OUT[P]
      E inExpr = nullptr;

      // Check if entry block
      bool isEntry = (&BB == &F.getEntryBlock());

      if (isEntry) {
        inExpr = Exp::term(boundary);
      } else {
        bool hasPreds = false;
        auto *First = BB.empty() ? nullptr : &BB.front();
        std::vector<llvm::BasicBlock *> PredBlocks;
        for (auto *PredInst : CFG.getPredsOf(
                 First, ::dataflow::controlflow::FlowDirection::Forward)) {
          auto *Pred = PredInst ? PredInst->getParent() : nullptr;
          if (Pred == nullptr || std::find(PredBlocks.begin(), PredBlocks.end(),
                                           Pred) != PredBlocks.end()) {
            continue;
          }
          PredBlocks.push_back(Pred);
        }
        for (auto *Pred : PredBlocks) {
          hasPreds = true;
          std::string predOut = getBlockSymbol(Pred, "OUT");
          auto pHole = Exp::hole(predOut);
          if (!inExpr)
            inExpr = pHole;
          else
            inExpr = Exp::ndet(inExpr, pHole);
        }
        if (!hasPreds) {
          // Unreachable block or just no preds in CFG?
          // Treat as empty (Zero)
          inExpr = Exp::term(D::zero());
        }
      }
      eqns.emplace_back(inSym, inExpr);

      // OUT[B] = GEN[B] U (IN[B] - KILL[B])
      // OUT[B] = GEN[B] U (IN[B] & ~KILL[B])
      // seq(c, t) -> extend(c, t) -> c & t
      auto outBody = Exp::seq(notKill, Exp::hole(inSym));
      auto outExpr = Exp::ndet(Exp::term(gen), outBody);
      eqns.emplace_back(outSym, outExpr);

    } else {
      // Backward Analysis
      // OUT[B] = U IN[S]
      E outExpr = nullptr;

      bool hasSuccs = false;
      auto *Term = BB.getTerminator();
      std::vector<llvm::BasicBlock *> SuccBlocks;
      for (auto *SuccInst : CFG.getSuccsOf(
               Term, ::dataflow::controlflow::FlowDirection::Forward)) {
        auto *Succ = SuccInst ? SuccInst->getParent() : nullptr;
        if (Succ == nullptr || std::find(SuccBlocks.begin(), SuccBlocks.end(),
                                         Succ) != SuccBlocks.end()) {
          continue;
        }
        SuccBlocks.push_back(Succ);
      }
      for (auto *Succ : SuccBlocks) {
        hasSuccs = true;
        std::string succIn = getBlockSymbol(Succ, "IN");
        auto sHole = Exp::hole(succIn);
        if (!outExpr)
          outExpr = sHole;
        else
          outExpr = Exp::ndet(outExpr, sHole);
      }

      if (!hasSuccs) {
        // Exit block
        outExpr = Exp::term(boundary);
      }

      eqns.emplace_back(outSym, outExpr);

      // IN[B] = GEN[B] U (OUT[B] - KILL[B])
      auto inBody = Exp::seq(notKill, Exp::hole(outSym));
      auto inExpr = Exp::ndet(Exp::term(gen), inBody);
      eqns.emplace_back(inSym, inExpr);
    }
  }

  // 3. Solve
  std::pair<std::vector<std::pair<Symbol, D::value_type>>, Stat> rawRes;
  if (strategy == SolverStrategy::Newton) {
    rawRes = NPASolver<D>::solve(eqns, verbose, -1, linearStrategy);
  } else {
    rawRes = KleeneSolver<D>::solve(eqns, verbose);
  }

  // 4. Parse Result
  Result result;
  result.stats = rawRes.second;

  std::unordered_map<std::string, llvm::APInt> rawMap;
  for (auto &p : rawRes.first) {
    rawMap[p.first] = p.second;
  }

  for (auto &BB : F) {
    std::string inSym = getBlockSymbol(&BB, "IN");
    std::string outSym = getBlockSymbol(&BB, "OUT");

    // Use 0 (Empty) if symbol missing (e.g. unreachable blocks might not be
    // solved if equations omitted? Actually we generated equations for all
    // blocks, so they should be there.
    if (rawMap.count(inSym))
      result.IN[&BB] = rawMap[inSym];
    else
      result.IN[&BB] = D::zero();

    if (rawMap.count(outSym))
      result.OUT[&BB] = rawMap[outSym];
    else
      result.OUT[&BB] = D::zero();
  }

  return result;
}

} // namespace npa
