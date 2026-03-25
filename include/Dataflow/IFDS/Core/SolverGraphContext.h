/*
 * Shared IFDS/IDE Graph Context
 *
 * Centralizes ICFG-derived call graph, return sites, CFG successors, and
 * initial seed construction so IFDS and IDE solvers do not duplicate it.
 */

#pragma once

#include "Dataflow/ControlFlow/InterCFG.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace ifds {

template <typename Fact, typename Problem> class SolverGraphContext {
public:
  using FactSet = typename Problem::FactSet;
  using InitialSeeds = typename IFDSProblem<Fact>::InitialSeeds;

  void initialize(const llvm::Module &module) {
    clear();

    m_icfg = std::make_unique<::dataflow::controlflow::LLVMInterCFG>(
        const_cast<llvm::Module *>(&module));

    for (const llvm::Function &func : module) {
      if (func.isDeclaration()) {
        continue;
      }

      for (const llvm::BasicBlock &bb : func) {
        for (const llvm::Instruction &inst : bb) {
          // CFG successors
          std::vector<const llvm::Instruction *> succs;
          for (auto *succ : m_icfg->getSuccsOf(
                   const_cast<llvm::Instruction *>(&inst),
                   ::dataflow::controlflow::FlowDirection::Forward)) {
            if (succ != nullptr) {
              succs.push_back(succ);
            }
          }
          m_successors[&inst] = succs;

          if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
            std::vector<const llvm::Function *> callees;
            std::unordered_set<const llvm::Function *> seen;
            for (const llvm::Function *callee :
                 m_icfg->getCalleesOfCallAt(const_cast<llvm::CallBase *>(call))) {
              if (!callee || !seen.insert(callee).second) {
                continue;
              }
              callees.push_back(callee);
              m_callee_to_calls[callee].push_back(call);
            }
            if (!callees.empty()) {
              m_call_to_callees[call] = std::move(callees);
            }
          }
        }
      }
    }
  }

  std::vector<const llvm::Instruction *>
  get_return_sites(const llvm::CallBase *call) const {
    if (!call) {
      return {};
    }
    auto it = m_successors.find(call);
    if (it != m_successors.end()) {
      return it->second;
    }
    if (!m_icfg) {
      return {};
    }
    std::vector<const llvm::Instruction *> out;
    for (auto *site :
         m_icfg->getReturnSitesOfCallAt(const_cast<llvm::CallBase *>(call))) {
      if (site != nullptr) {
        out.push_back(site);
      }
    }
    return out;
  }

  std::vector<const llvm::Instruction *>
  get_successors(const llvm::Instruction *inst) const {
    auto it = m_successors.find(inst);
    if (it != m_successors.end()) {
      return it->second;
    }
    return {};
  }

  InitialSeeds build_initial_seeds(Problem &problem,
                                   const llvm::Module &module) const {
    return problem.initial_seeds(module);
  }

  void clear() {
    m_call_to_callees.clear();
    m_callee_to_calls.clear();
    m_successors.clear();
    m_icfg.reset();
  }

  const std::unordered_map<const llvm::CallBase *,
                           std::vector<const llvm::Function *>> &
  call_to_callees() const {
    return m_call_to_callees;
  }

  const std::unordered_map<const llvm::Function *,
                           std::vector<const llvm::CallBase *>> &
  callee_to_calls() const {
    return m_callee_to_calls;
  }

private:
  std::unique_ptr<::dataflow::controlflow::LLVMInterCFG> m_icfg;
  std::unordered_map<const llvm::CallBase *, std::vector<const llvm::Function *>>
      m_call_to_callees;
  std::unordered_map<const llvm::Function *, std::vector<const llvm::CallBase *>>
      m_callee_to_calls;
  std::unordered_map<const llvm::Instruction *,
                     std::vector<const llvm::Instruction *>>
      m_successors;
};

} // namespace ifds
