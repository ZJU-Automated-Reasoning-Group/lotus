/*
 *
 * Author: rainoftime
 */
#include "Dataflow/ControlFlow/InterCFG.h"
#include "Dataflow/WPDS/InterProceduralDataFlow.h"
#include "Solvers/WPDS/CA.h"
#include "Solvers/WPDS/SaturationProcess.h"
#ifdef WITNESS_TRACE
#include "Solvers/WPDS/Witness.h"
#endif
#include "llvm/ADT/Optional.h"
#include "llvm/IR/CFG.h"

#include <sstream>
#include <unordered_map>

namespace wpds {

using namespace wpds;
using namespace llvm;

static bool isValueInInstructionScope(Value *v, const Function *f) {
  if (v == nullptr) {
    return false;
  }
  if (isa<GlobalValue>(v) || isa<Constant>(v)) {
    return true;
  }
  if (const auto *a = dyn_cast<Argument>(v)) {
    return a->getParent() == f;
  }
  if (const auto *i = dyn_cast<Instruction>(v)) {
    return i->getFunction() == f;
  }
  return true;
}

static void filterFactsToInstructionScope(Instruction *inst,
                                          std::set<Value *> &facts) {
  if (inst == nullptr) {
    facts.clear();
    return;
  }
  Function *f = inst->getFunction();
  if (f == nullptr) {
    return;
  }
  for (auto it = facts.begin(); it != facts.end();) {
    if (!isValueInInstructionScope(*it, f)) {
      it = facts.erase(it);
    } else {
      ++it;
    }
  }
}

InterProceduralDataFlowEngine::InterProceduralDataFlowEngine()
    : controlState(str2key("q")) {}

void InterProceduralDataFlowEngine::setCalleeResolver(CalleeResolver resolver) {
  calleeResolver = std::move(resolver);
}

void InterProceduralDataFlowEngine::setExternalCallPolicy(
    ExternalCallPolicy policy) {
  externalCallPolicy = std::move(policy);
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runForwardAnalysis(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const std::set<Value *> &initialFacts) {
  return runForwardAnalysisWithAutomaton(
      m, createTransformer, [&](CA<GenKillTransformer> &ca) {
        buildInitialAutomaton(m, ca, initialFacts, true);
      });
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runBackwardAnalysis(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const std::set<Value *> &initialFacts) {
  return runBackwardAnalysisWithAutomaton(
      m, createTransformer, [&](CA<GenKillTransformer> &ca) {
        buildInitialAutomaton(m, ca, initialFacts, false);
      });
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runForwardAnalysisWithAutomaton(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const AutomatonBuilder &buildInitialCA) {
  return runAnalysisWithAutomaton(m, createTransformer, buildInitialCA, true);
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runForwardAnalysisFromEntries(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const std::vector<Function *> &entryFunctions,
    const std::set<Value *> &initialFacts) {
  return runForwardAnalysisWithAutomaton(
      m, createTransformer, [&](CA<GenKillTransformer> &ca) {
        buildSeedAutomatonForFunctions(ca, entryFunctions, initialFacts, false);
      });
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runBackwardAnalysisWithAutomaton(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const AutomatonBuilder &buildInitialCA) {
  return runAnalysisWithAutomaton(m, createTransformer, buildInitialCA, false);
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runBackwardAnalysisFromExits(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const std::vector<Function *> &exitFunctions,
    const std::set<Value *> &initialFacts) {
  return runBackwardAnalysisWithAutomaton(
      m, createTransformer, [&](CA<GenKillTransformer> &ca) {
        buildSeedAutomatonForFunctions(ca, exitFunctions, initialFacts, true);
      });
}

std::unique_ptr<mono::DataFlowResult>
InterProceduralDataFlowEngine::runAnalysisWithAutomaton(
    Module &m,
    const std::function<GenKillTransformer *(Instruction *)> &createTransformer,
    const AutomatonBuilder &buildInitialCA, bool isForward) {
  // Model both directions as forward reachability over direction-specific
  // program graphs. This keeps interprocedural call/return wiring explicit.
  Semiring<GenKillTransformer> semiring(GenKillTransformer::one(), true);
  WPDS<GenKillTransformer> wpds(semiring, Query::poststar());

  // Build WPDS from LLVM module
  buildWPDS(m, wpds, createTransformer, isForward);

  // Build initial configuration automaton (in-place)
  CA<GenKillTransformer> resultCA(semiring);
  lastAcceptState = llvm::None;
  buildInitialCA(resultCA);

  // Run saturation algorithm
  wpds::SaturationProcess<GenKillTransformer> satProcess(
      wpds, resultCA, semiring, Query::poststar());
  satProcess.poststar();

  // Extract results
  currentResult = std::make_unique<mono::DataFlowResult>();
  extractResults(m, resultCA, currentResult, isForward);

  // Cache for queries/witnesses
  lastResultCA = std::make_unique<CA<GenKillTransformer>>(resultCA);
  lastQuery = Query::poststar();

  return std::make_unique<mono::DataFlowResult>(*currentResult);
}

const std::set<Value *> &
InterProceduralDataFlowEngine::getInSet(Instruction *inst) const {
  if (!currentResult) {
    static std::set<Value *> emptySet;
    return emptySet;
  }
  return currentResult->IN(inst);
}

const std::set<Value *> &
InterProceduralDataFlowEngine::getOutSet(Instruction *inst) const {
  if (!currentResult) {
    static std::set<Value *> emptySet;
    return emptySet;
  }
  return currentResult->OUT(inst);
}

std::set<Value *>
InterProceduralDataFlowEngine::queryFactsBeforeInstruction(
    Instruction *inst) const {
  std::set<Value *> facts =
      queryFactsAtSymbol(getProgramPointKeyBeforeInstruction(inst));
  filterFactsToInstructionScope(inst, facts);
  return facts;
}

std::set<Value *>
InterProceduralDataFlowEngine::queryFactsAfterInstruction(
    Instruction *inst) const {
  std::set<Value *> facts =
      queryFactsAtSymbol(getProgramPointKeyAfterInstruction(inst));
  filterFactsToInstructionScope(inst, facts);
  return facts;
}

::ref_ptr<GenKillTransformer>
InterProceduralDataFlowEngine::querySummaryBeforeInstruction(
    Instruction *inst) const {
  return querySummaryAtSymbol(getProgramPointKeyBeforeInstruction(inst));
}

::ref_ptr<GenKillTransformer>
InterProceduralDataFlowEngine::querySummaryAfterInstruction(
    Instruction *inst) const {
  return querySummaryAtSymbol(getProgramPointKeyAfterInstruction(inst));
}

wpds::wpds_key_t
InterProceduralDataFlowEngine::getProgramPointKeyBeforeInstruction(
    Instruction *inst) const {
  auto it = instPrevKey.find(inst);
  return it != instPrevKey.end() ? it->second : WPDS_EPSILON;
}

wpds::wpds_key_t
InterProceduralDataFlowEngine::getProgramPointKeyAfterInstruction(
    Instruction *inst) const {
  if (auto *callInst = dyn_cast_or_null<CallBase>(inst)) {
    auto retIt = callReturnToKey.find(callInst);
    if (retIt != callReturnToKey.end()) {
      return retIt->second;
    }
  }
  auto it = instToKey.find(inst);
  return it != instToKey.end() ? it->second : WPDS_EPSILON;
}

void InterProceduralDataFlowEngine::buildWPDS(
    Module &m, WPDS<GenKillTransformer> &wpds,
    const std::function<GenKillTransformer *(Instruction *)>
        &createTransformer,
    bool isForward) {
  ::dataflow::controlflow::LLVMIntraCFG intraCfg;
  std::unique_ptr<::dataflow::controlflow::LLVMInterCFG> interCfgStorage;
  if (calleeResolver) {
    interCfgStorage = std::make_unique<::dataflow::controlflow::LLVMInterCFG>(
        &m, [this](Instruction *inst) -> std::vector<Function *> {
          auto *call = dyn_cast<CallBase>(inst);
          return call ? calleeResolver(call) : std::vector<Function *>{};
        });
  } else {
    interCfgStorage =
        std::make_unique<::dataflow::controlflow::LLVMInterCFG>(&m);
  }
  auto &interCfg = *interCfgStorage;

  // Clear previous mappings
  functionToKey.clear();
  functionExitToKey.clear();
  instToKey.clear();
  instPrevKey.clear();
  bbToKey.clear();
  callReturnToKey.clear();
  keyToInst.clear();
  localGenByInst.clear();
  localKillByInst.clear();

  auto functionTag = [&](Function &F) -> std::string {
    std::string fname = F.getName().str();
    if (fname.empty()) {
      fname = "anon";
    }
    return fname + "_" + std::to_string((uintptr_t)&F);
  };

  auto bbTag = [&](Function &F, BasicBlock &BB) -> std::string {
    std::string name = BB.getName().str();
    if (name.empty()) {
      name = "bb";
    }
    return functionTag(F) + "_bb_" + name + "_" +
           std::to_string((uintptr_t)&BB);
  };

  auto instTag = [&](Function &F, Instruction &I) -> std::string {
    std::string name = I.getName().str();
    if (name.empty()) {
      name = "inst";
    }
    return functionTag(F) + "_i_" + name + "_" + std::to_string((uintptr_t)&I);
  };

  auto retTag = [&](Function &F, Instruction &callI) -> std::string {
    return functionTag(F) + "_ret_" + std::to_string((uintptr_t)&callI);
  };

  auto ensurePopRule = [&](wpds_key_t boundaryKey) {
    wpds.add_rule(controlState, boundaryKey, controlState,
                  GenKillTransformer::one());
  };

  auto getAfterKey = [&](Instruction *inst) -> wpds_key_t {
    if (auto *callInst = dyn_cast_or_null<CallBase>(inst)) {
      auto retIt = callReturnToKey.find(callInst);
      if (retIt != callReturnToKey.end()) {
        return retIt->second;
      }
    }
    auto instIt = instToKey.find(inst);
    return instIt != instToKey.end() ? instIt->second : WPDS_EPSILON;
  };

  // First pass: Create function entry and exit keys for all functions
  for (auto &F : m) {
    if (F.isDeclaration())
      continue;

    const std::string ftag = functionTag(F);
    wpds_key_t funcEntry = new_str2key(("entry_" + ftag).c_str());
    wpds_key_t funcExit = new_str2key(("exit_" + ftag).c_str());
    functionToKey[&F] = funcEntry;
    functionExitToKey[&F] = funcExit;

    wpds.add_element_to_P(controlState);
  }

  // Second pass: assign basic-block and instruction keys.
  for (auto &F : m) {
    if (F.isDeclaration())
      continue;

    for (auto &BB : F) {
      const std::string btag = bbTag(F, BB);
      wpds_key_t bbKey = new_str2key(btag.c_str());
      bbToKey[&BB] = bbKey;
      wpds_key_t prevKey = bbKey;
      for (auto &I : BB) {
        const std::string itag = instTag(F, I);
        wpds_key_t instKey = new_str2key(itag.c_str());
        instToKey[&I] = instKey;
        keyToInst[instKey] = &I;
        instPrevKey[&I] = prevKey;
        if (auto *callInst = dyn_cast<CallBase>(&I)) {
          callReturnToKey[callInst] = new_str2key(retTag(F, I).c_str());
        }
        prevKey = getAfterKey(&I);
      }
    }
  }

  // Third pass: create direction-specific rules.
  for (auto &F : m) {
    if (F.isDeclaration()) {
      continue;
    }

    wpds_key_t funcEntry = functionToKey[&F];
    wpds_key_t funcExit = functionExitToKey[&F];
    BasicBlock &entryBB = F.getEntryBlock();
    wpds_key_t entryBBKey = bbToKey[&entryBB];

    if (isForward) {
      wpds.add_rule(controlState, funcEntry, controlState, entryBBKey,
                    GenKillTransformer::one());
    } else {
      wpds.add_rule(controlState, entryBBKey, controlState, funcEntry,
                    GenKillTransformer::one());
    }

    for (auto &BB : F) {
      wpds_key_t bbKey = bbToKey[&BB];
      if (!isForward && &BB != &entryBB) {
        for (BasicBlock *pred : predecessors(&BB)) {
          if (pred == nullptr || pred->getTerminator() == nullptr) {
            continue;
          }
          wpds.add_rule(controlState, bbKey, controlState,
                        getAfterKey(pred->getTerminator()),
                        GenKillTransformer::one());
        }
      }

      for (auto &I : BB) {
        wpds_key_t beforeKey = instPrevKey[&I];
        wpds_key_t instKey = instToKey[&I];
        wpds_key_t afterKey = getAfterKey(&I);

        GenKillTransformer *transformer = createTransformer(&I);
        if (!transformer) {
          transformer = GenKillTransformer::one();
        }
        localGenByInst[&I] = transformer->getGen().getFacts();
        localKillByInst[&I] = transformer->getKill().getFacts();

        if (isForward) {
          wpds.add_rule(controlState, beforeKey, controlState, instKey,
                        transformer);
        } else {
          wpds.add_rule(controlState, instKey, controlState, beforeKey,
                        transformer);
        }

        if (auto *callInst = dyn_cast<CallBase>(&I)) {
          std::vector<Function *> callees;
          if (calleeResolver) {
            callees = calleeResolver(callInst);
          } else {
            callees = interCfg.getCalleesOfCallAt(callInst);
          }

          bool hasModeledCallee = false;
          bool hasUnmodeledCallee = callees.empty();

          auto connectForwardReturnJoin = [&](wpds_key_t pathReturnKey,
                                              GenKillTransformer *weight) {
            wpds.add_rule(controlState, pathReturnKey, controlState, afterKey,
                          weight ? weight : GenKillTransformer::one());
          };
          auto connectBackwardCallSite = [&](wpds_key_t pathCallKey,
                                             GenKillTransformer *weight) {
            wpds.add_rule(controlState, pathCallKey, controlState, instKey,
                          weight ? weight : GenKillTransformer::one());
          };

          for (Function *calledFunc : callees) {
            if (!calledFunc || calledFunc->isDeclaration() ||
                functionToKey.find(calledFunc) == functionToKey.end()) {
              hasUnmodeledCallee = true;
              continue;
            }

            hasModeledCallee = true;
            wpds_key_t calledEntry = functionToKey[calledFunc];
            wpds_key_t calledExit = functionExitToKey[calledFunc];

            std::map<Value *, DataFlowFacts> actualToFormalFlow;
            std::map<Value *, DataFlowFacts> formalToActualFlow;
            unsigned argIdx = 0;
            for (auto &formal : calledFunc->args()) {
              if (argIdx < callInst->arg_size()) {
                Value *actual = callInst->getArgOperand(argIdx);
                actualToFormalFlow[actual].addFact(&formal);
                formalToActualFlow[&formal].addFact(actual);
              }
              argIdx++;
            }

            std::map<Value *, DataFlowFacts> retToCallFlow;
            std::map<Value *, DataFlowFacts> callToRetFlow;
            if (!callInst->getType()->isVoidTy()) {
              for (auto &calleeBB : *calledFunc) {
                if (auto *retInst = dyn_cast<ReturnInst>(calleeBB.getTerminator())) {
                  Value *rv = retInst->getReturnValue();
                  if (rv == nullptr) {
                    continue;
                  }
                  retToCallFlow[rv].addFact(callInst);
                  callToRetFlow[callInst].addFact(rv);
                }
              }
            }

            if (isForward) {
              wpds_key_t pathReturnKey =
                  new_str2key(
                      (retTag(F, I) + "_callee_" + functionTag(*calledFunc))
                          .c_str());
              wpds.add_rule(
                  controlState, instKey, controlState, calledEntry, pathReturnKey,
                  GenKillTransformer::makeGenKillTransformer(
                      DataFlowFacts::EmptySet(), DataFlowFacts::EmptySet(),
                      actualToFormalFlow));
              ensurePopRule(calledExit);
              connectForwardReturnJoin(
                  pathReturnKey,
                  GenKillTransformer::makeGenKillTransformer(
                      DataFlowFacts::EmptySet(), DataFlowFacts::EmptySet(),
                      retToCallFlow));
            } else {
              wpds_key_t pathCallKey =
                  new_str2key((retTag(F, I) + "_callee_back_" +
                               functionTag(*calledFunc))
                                  .c_str());
              wpds.add_rule(
                  controlState, afterKey, controlState, calledExit, pathCallKey,
                  GenKillTransformer::makeGenKillTransformer(
                      DataFlowFacts::EmptySet(), DataFlowFacts::EmptySet(),
                      callToRetFlow));
              ensurePopRule(calledEntry);
              connectBackwardCallSite(
                  pathCallKey,
                  GenKillTransformer::makeGenKillTransformer(
                      DataFlowFacts::EmptySet(), DataFlowFacts::EmptySet(),
                      formalToActualFlow));
            }
          }

          if (hasUnmodeledCallee) {
            wpds_key_t unknownKey = new_str2key(
                (retTag(F, I) + (isForward ? "_unknown" : "_unknown_back"))
                    .c_str());
            if (isForward) {
              wpds.add_rule(controlState, instKey, controlState, unknownKey,
                            buildUnknownCallSummary(callInst, m, true));
              connectForwardReturnJoin(unknownKey, GenKillTransformer::one());
            } else {
              wpds.add_rule(controlState, afterKey, controlState, unknownKey,
                            buildUnknownCallSummary(callInst, m, false));
              connectBackwardCallSite(unknownKey, GenKillTransformer::one());
            }
          }

          if (isForward && (hasModeledCallee || hasUnmodeledCallee) &&
              I.isTerminator()) {
            for (auto *retSite : interCfg.getReturnSitesOfCallAt(callInst)) {
              if (retSite == nullptr) {
                continue;
              }
              auto bbIt = bbToKey.find(retSite->getParent());
              if (bbIt == bbToKey.end()) {
                continue;
              }
              wpds.add_rule(controlState, afterKey, controlState, bbIt->second,
                            GenKillTransformer::one());
            }
          }
          continue;
        }

        if (isa<ReturnInst>(&I)) {
          if (isForward) {
            wpds.add_rule(controlState, instKey, controlState, funcExit,
                          GenKillTransformer::one());
          } else {
            wpds.add_rule(controlState, funcExit, controlState, instKey,
                          GenKillTransformer::one());
          }
          continue;
        }
      }

      if (isForward) {
        if (Instruction *terminator = BB.getTerminator()) {
          wpds_key_t termKey = getAfterKey(terminator);
          if (!isa<ReturnInst>(terminator) && !isa<CallBase>(terminator)) {
            for (auto *succInst : intraCfg.getSuccsOf(
                     terminator,
                     ::dataflow::controlflow::FlowDirection::Forward)) {
              if (succInst == nullptr) {
                continue;
              }
              wpds.add_rule(controlState, termKey, controlState,
                            bbToKey[succInst->getParent()],
                            GenKillTransformer::one());
            }
          }
        }
      }
    }
  }
}

void InterProceduralDataFlowEngine::buildInitialAutomaton(
    Module &m, CA<GenKillTransformer> &ca,
    const std::set<Value *> &initialFacts, bool isForward) {

  wpds_key_t acceptState = str2key("accept");
  lastAcceptState = acceptState;

  ca.add_initial_state(controlState);
  ca.add_final_state(acceptState);

  if (isForward) {
    // For forward analysis: start from main if present, otherwise seed all
    // entries.
    Function *mainFn = nullptr;
    for (auto &F : m) {
      if (F.isDeclaration())
        continue;
      if (F.getName() == "main") {
        mainFn = &F;
        break;
      }
    }
    GenKillTransformer *initTrans = GenKillTransformer::makeGenKillTransformer(
        DataFlowFacts::EmptySet(), DataFlowFacts(initialFacts));
    if (mainFn) {
      ca.add(controlState, functionToKey[mainFn], acceptState, initTrans);
    } else {
      for (auto &kv : functionToKey) {
        ca.add(controlState, kv.second, acceptState, initTrans);
      }
    }
  } else {
    // For backward analysis: start from all exit points
    for (auto &kv : functionExitToKey) {
      wpds_key_t exitKey = kv.second;

      GenKillTransformer *initTrans =
          GenKillTransformer::makeGenKillTransformer(
              DataFlowFacts::EmptySet(), DataFlowFacts(initialFacts));

      ca.add(controlState, exitKey, acceptState, initTrans);
    }
  }
}

void InterProceduralDataFlowEngine::buildSeedAutomatonForFunctions(
    CA<GenKillTransformer> &ca, const std::vector<Function *> &functions,
    const std::set<Value *> &initialFacts, bool useExitPoints) {
  wpds_key_t acceptState = str2key("accept");
  lastAcceptState = acceptState;

  ca.add_initial_state(controlState);
  ca.add_final_state(acceptState);

  GenKillTransformer *initTrans = GenKillTransformer::makeGenKillTransformer(
      DataFlowFacts::EmptySet(), DataFlowFacts(initialFacts));

  for (Function *function : functions) {
    if (function == nullptr) {
      continue;
    }
    auto &keyMap = useExitPoints ? functionExitToKey : functionToKey;
    auto it = keyMap.find(function);
    if (it == keyMap.end()) {
      continue;
    }
    ca.add(controlState, it->second, acceptState, initTrans);
  }
}

wpds_key_t InterProceduralDataFlowEngine::getKeyForFunction(Function *f) {
  auto it = functionToKey.find(f);
  if (it != functionToKey.end()) {
    return it->second;
  }
  return WPDS_EPSILON;
}

wpds_key_t
InterProceduralDataFlowEngine::getKeyForInstruction(Instruction *inst) {
  auto it = instToKey.find(inst);
  if (it != instToKey.end()) {
    return it->second;
  }
  return WPDS_EPSILON;
}

wpds_key_t InterProceduralDataFlowEngine::getKeyForBasicBlock(BasicBlock *bb) {
  auto it = bbToKey.find(bb);
  if (it != bbToKey.end()) {
    return it->second;
  }
  return WPDS_EPSILON;
}

wpds_key_t
InterProceduralDataFlowEngine::getKeyForCallSite(CallBase *callInst) {
  return getKeyForInstruction(callInst);
}

wpds_key_t
InterProceduralDataFlowEngine::getKeyForReturnSite(CallBase *callInst) {
  auto it = callReturnToKey.find(callInst);
  if (it != callReturnToKey.end()) {
    return it->second;
  }
  return WPDS_EPSILON;
}

void InterProceduralDataFlowEngine::extractResults(
    Module &m, CA<GenKillTransformer> &resultCA,
    std::unique_ptr<mono::DataFlowResult> &result, bool isForward) {
  (void)m;
  (void)isForward;

  wpds_key_t queryInit = resultCA.initial_state();
  if (queryInit == WPDS_EPSILON) {
    queryInit = controlState;
  }

  // Cache value-at-symbol queries to avoid repeated reglang_query work.
  struct KeyQueryResult {
    std::set<Value *> facts;
    llvm::Optional<std::set<Value *>> gen;
    llvm::Optional<std::set<Value *>> kill;
  };
  std::unordered_map<wpds_key_t, KeyQueryResult> cache;

  auto querySymbol = [&](wpds_key_t sym,
                         bool wantGenKill) -> const KeyQueryResult & {
    auto it = cache.find(sym);
    if (it != cache.end() && (!wantGenKill || (it->second.gen.hasValue() &&
                                               it->second.kill.hasValue()))) {
      return it->second;
    }

    // Query the regular language consisting of the single stack symbol `sym`.
    CA<GenKillTransformer> lang(resultCA.semiring());
    wpds_key_t qf =
        new_str2key(("query_final_" + std::to_string((uintptr_t)sym)).c_str());
    lang.add_initial_state(queryInit);
    lang.add_final_state(qf);
    lang.add(queryInit, sym, qf, GenKillTransformer::one());

    auto pathSummary = resultCA.reglang_query(lang);
    KeyQueryResult res;
    // Treat zero (no path / empty intersection) explicitly so we don't rely on
    // zero()->apply semantics.
    if (pathSummary.get_ptr() &&
        !pathSummary->equal(GenKillTransformer::zero())) {
      DataFlowFacts outFacts = pathSummary->apply(DataFlowFacts::EmptySet());
      res.facts = outFacts.getFacts();
      if (wantGenKill) {
        res.gen = pathSummary->getGen().getFacts();
        res.kill = pathSummary->getKill().getFacts();
      }
    }

    if (it == cache.end()) {
      cache.emplace(sym, std::move(res));
      return cache.find(sym)->second;
    }
    it->second = std::move(res);
    return it->second;
  };

  // Compute IN/OUT directly from the saturated automaton.
  for (auto &kv : instToKey) {
    Instruction *inst = kv.first;
    wpds_key_t afterKey = getProgramPointKeyAfterInstruction(inst);

    // OUT at instruction = value at the "after-inst" program-point symbol.
    result->OUT(inst) = querySymbol(afterKey, /*wantGenKill=*/true).facts;
    filterFactsToInstructionScope(inst, result->OUT(inst));

    // IN at instruction = value at the program-point symbol that precedes the
    // instruction.
    auto pkIt = instPrevKey.find(inst);
    if (pkIt != instPrevKey.end()) {
      result->IN(inst) = querySymbol(pkIt->second, /*wantGenKill=*/false).facts;
      filterFactsToInstructionScope(inst, result->IN(inst));
    }

    auto genIt = localGenByInst.find(inst);
    if (genIt != localGenByInst.end()) {
      result->GEN(inst) = genIt->second;
      filterFactsToInstructionScope(inst, result->GEN(inst));
    }
    auto killIt = localKillByInst.find(inst);
    if (killIt != localKillByInst.end()) {
      result->KILL(inst) = killIt->second;
      filterFactsToInstructionScope(inst, result->KILL(inst));
    }
  }
}

const wpds::CA<GenKillTransformer> *
InterProceduralDataFlowEngine::getLastResultAutomaton() const {
  return lastResultCA.get();
}

::ref_ptr<GenKillTransformer>
InterProceduralDataFlowEngine::querySummaryAtSymbol(wpds::wpds_key_t symbol) const {
  if (!lastResultCA || symbol == WPDS_EPSILON) {
    return ::ref_ptr<GenKillTransformer>(GenKillTransformer::zero());
  }

  wpds_key_t queryInit = lastResultCA->initial_state();
  if (queryInit == WPDS_EPSILON) {
    queryInit = controlState;
  }

  CA<GenKillTransformer> lang(lastResultCA->semiring());
  wpds_key_t qf =
      new_str2key(("query_final_" + std::to_string((uintptr_t)symbol)).c_str());
  lang.add_initial_state(queryInit);
  lang.add_final_state(qf);
  lang.add(queryInit, symbol, qf, GenKillTransformer::one());
  return lastResultCA->reglang_query(lang);
}

std::set<Value *>
InterProceduralDataFlowEngine::queryFactsAtSymbol(wpds::wpds_key_t symbol) const {
  auto summary = querySummaryAtSymbol(symbol);
  if (!summary.get_ptr() || summary->equal(GenKillTransformer::zero())) {
    return {};
  }
  DataFlowFacts facts = summary->apply(DataFlowFacts::EmptySet());
  return facts.getFacts();
}

::ref_ptr<GenKillTransformer>
InterProceduralDataFlowEngine::queryRegularLanguage(
    const wpds::CA<GenKillTransformer> &lang) const {
  if (!lastResultCA) {
    return ::ref_ptr<GenKillTransformer>(GenKillTransformer::zero());
  }
  return lastResultCA->reglang_query(lang);
}

GenKillTransformer *InterProceduralDataFlowEngine::buildUnknownCallSummary(
    CallBase *callInst, Module &m, bool isForward) const {
  if (callInst == nullptr) {
    return GenKillTransformer::one();
  }

  std::vector<Value *> pointerObjects =
      MemoryObjectFact::pointerArgumentObjects(callInst);
  std::vector<GlobalValue *> globals = MemoryObjectFact::trackedGlobals(m);

  if (externalCallPolicy.buildSummary) {
    if (GenKillTransformer *custom =
            externalCallPolicy.buildSummary(callInst, pointerObjects, globals)) {
      return custom;
    }
  }

  DataFlowFacts killFacts = DataFlowFacts::EmptySet();
  std::set<Value *> genSet;
  std::map<Value *, DataFlowFacts> flow;
  if (!externalCallPolicy.preserveIdentity) {
    for (Value *object : pointerObjects) {
      MemoryObjectFact::addRepresentativeFact(killFacts, object);
    }
    for (GlobalValue *global : globals) {
      MemoryObjectFact::addRepresentativeFact(killFacts, global);
    }
  }

  if (!callInst->getType()->isVoidTy()) {
    const bool mayFlowFromPointers =
        externalCallPolicy.flowPointerArgumentsToReturn && !pointerObjects.empty();
    const bool mayFlowFromGlobals =
        externalCallPolicy.flowGlobalsToReturn && !globals.empty();
    const bool mayGenerateReturn = mayFlowFromPointers || mayFlowFromGlobals;

    if (isForward) {
      if (mayFlowFromPointers && externalCallPolicy.preserveIdentity) {
        for (Value *object : pointerObjects) {
          MemoryObjectFact::addFlow(flow, object, callInst);
        }
      }
      if (mayFlowFromGlobals && externalCallPolicy.preserveIdentity) {
        for (GlobalValue *global : globals) {
          MemoryObjectFact::addFlow(flow, global, callInst);
        }
      }
      if (!externalCallPolicy.preserveIdentity && mayGenerateReturn) {
        genSet.insert(callInst);
      }
    } else {
      if (mayFlowFromPointers) {
        for (Value *object : pointerObjects) {
          MemoryObjectFact::addFlow(flow, callInst, object);
        }
      }
      if (mayFlowFromGlobals) {
        for (GlobalValue *global : globals) {
          MemoryObjectFact::addFlow(flow, callInst, global);
        }
      }
    }

  }

  return GenKillTransformer::makeGenKillTransformer(
      killFacts, DataFlowFacts(genSet), flow);
}

#ifdef WITNESS_TRACE
std::string InterProceduralDataFlowEngine::getWitnessDagDotForTransition(
    wpds::wpds_key_t from, wpds::wpds_key_t stack, wpds::wpds_key_t to) const {
  if (!lastResultCA) {
    return "";
  }
  wpds::CA<GenKillTransformer>::catrans_t trans;
  if (!lastResultCA->find(from, stack, to, trans) || !trans.get_ptr()) {
    return "";
  }
  auto wit = trans->witness();
  if (!wit.get_ptr()) {
    return "";
  }

  using witness_path_t =
      wpds::ref_ptr<wpds::CAPathOfWitness<GenKillTransformer>>;
  witness_path_t path(
      new wpds::CAPathOfWitness<GenKillTransformer>(wit, witness_path_t(0)));
  auto dag =
      wpds::DAGWitnessForPath<GenKillTransformer>::createFromCAPathOfWitness(
          path, lastQuery);
  std::ostringstream oss;
  dag->print(oss);
  return oss.str();
}

std::string InterProceduralDataFlowEngine::getWitnessDagDotForInstruction(
    Instruction *inst) const {
  if (!lastAcceptState.hasValue()) {
    return "";
  }
  auto it = instToKey.find(inst);
  if (it == instToKey.end()) {
    return "";
  }
  return getWitnessDagDotForTransition(controlState, it->second,
                                       *lastAcceptState);
}
#endif

} // namespace wpds
