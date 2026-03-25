#ifndef ANALYSIS_DATAFLOW_WPDS_INTERPROCEDURALDATAFLOWENGINE_H_
#define ANALYSIS_DATAFLOW_WPDS_INTERPROCEDURALDATAFLOWENGINE_H_

#include "llvm/ADT/Optional.h"

#include "Dataflow/Mono/Support/Result.h"
#include "Dataflow/WPDS/Core/GenKillTransformer.h"
#include "Dataflow/WPDS/Core/MemoryObjectFact.h"
#include "Solvers/WPDS/CA.h"
#include "Solvers/WPDS/WPDS.h"
#include "Solvers/WPDS/key_source.h"
#include "Solvers/WPDS/keys.h"
#include "Solvers/WPDS/ref_ptr.h"
#include "Solvers/WPDS/semiring.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace wpds {

/**
 * Engine for interprocedural dataflow analysis via weighted PDS (WPDS).
 *
 * Encodes a program's supergraph as a WPDS (Section 4 of the paper), runs
 * backward or forward saturation (GPR / Algorithm 1), and extracts concrete
 * IN/OUT fact sets per instruction for may analyses. The regular-language
 * query interface is an expert API over the saturated configuration automaton.
 *
 * @see Reps, Schwoon, Jha: "Weighted Pushdown Systems and their Application
 *      to Interprocedural Dataflow Analysis"
 */
class InterProceduralDataFlowEngine {
public:
  using AutomatonBuilder = std::function<void(wpds::CA<GenKillTransformer> &)>;
  using CalleeResolver = std::function<std::vector<Function *>(CallBase *)>;

  struct ExternalCallPolicy {
    bool preserveIdentity = true;
    bool flowPointerArgumentsToReturn = true;
    bool flowGlobalsToReturn = false;
    std::function<GenKillTransformer *(
        CallBase *, const std::vector<Value *> &,
        const std::vector<GlobalValue *> &)>
        buildSummary;
  };

  InterProceduralDataFlowEngine();
  ~InterProceduralDataFlowEngine() = default;

  void setCalleeResolver(CalleeResolver resolver);
  void setExternalCallPolicy(ExternalCallPolicy policy);

  // Main method to run forward inter-procedural dataflow analysis
  std::unique_ptr<mono::DataFlowResult>
  runForwardAnalysis(Module &m,
                     const std::function<GenKillTransformer *(Instruction *)>
                         &createTransformer,
                     const std::set<Value *> &initialFacts = {});

  // Run forward analysis with a caller-provided initial configuration automaton
  std::unique_ptr<mono::DataFlowResult> runForwardAnalysisWithAutomaton(
      Module &m,
      const std::function<GenKillTransformer *(Instruction *)>
          &createTransformer,
      const AutomatonBuilder &buildInitialCA);

  std::unique_ptr<mono::DataFlowResult> runForwardAnalysisFromEntries(
      Module &m,
      const std::function<GenKillTransformer *(Instruction *)>
          &createTransformer,
      const std::vector<Function *> &entryFunctions,
      const std::set<Value *> &initialFacts = {});

  // Main method to run backward inter-procedural dataflow analysis
  std::unique_ptr<mono::DataFlowResult>
  runBackwardAnalysis(Module &m,
                      const std::function<GenKillTransformer *(Instruction *)>
                          &createTransformer,
                      const std::set<Value *> &initialFacts = {});

  // Run backward analysis with a caller-provided initial configuration
  // automaton
  std::unique_ptr<mono::DataFlowResult> runBackwardAnalysisWithAutomaton(
      Module &m,
      const std::function<GenKillTransformer *(Instruction *)>
          &createTransformer,
      const AutomatonBuilder &buildInitialCA);

  std::unique_ptr<mono::DataFlowResult> runBackwardAnalysisFromExits(
      Module &m,
      const std::function<GenKillTransformer *(Instruction *)>
          &createTransformer,
      const std::vector<Function *> &exitFunctions,
      const std::set<Value *> &initialFacts = {});

  // Helper methods to query results
  const std::set<Value *> &getInSet(Instruction *inst) const;
  const std::set<Value *> &getOutSet(Instruction *inst) const;
  std::set<Value *> queryFactsBeforeInstruction(Instruction *inst) const;
  std::set<Value *> queryFactsAfterInstruction(Instruction *inst) const;
  ::ref_ptr<GenKillTransformer> querySummaryBeforeInstruction(
      Instruction *inst) const;
  ::ref_ptr<GenKillTransformer> querySummaryAfterInstruction(
      Instruction *inst) const;
  wpds::wpds_key_t getProgramPointKeyBeforeInstruction(Instruction *inst) const;
  wpds::wpds_key_t getProgramPointKeyAfterInstruction(Instruction *inst) const;

  // Access the last saturated configuration automaton (if any)
  const wpds::CA<GenKillTransformer> *getLastResultAutomaton() const;

  // Query a regular language of stack configurations against the last automaton
  ::ref_ptr<GenKillTransformer>
  queryRegularLanguage(const wpds::CA<GenKillTransformer> &lang) const;

#ifdef WITNESS_TRACE
  // Return a DOT graph for the witness DAG of a specific transition
  std::string getWitnessDagDotForTransition(wpds::wpds_key_t from,
                                            wpds::wpds_key_t stack,
                                            wpds::wpds_key_t to) const;

  // Convenience: return DOT graph for the witness DAG of an instruction query
  // in the default automaton (controlState -> instKey -> accept)
  std::string getWitnessDagDotForInstruction(Instruction *inst) const;
#endif

private:
  std::unique_ptr<mono::DataFlowResult> runAnalysisWithAutomaton(
      Module &m,
      const std::function<GenKillTransformer *(Instruction *)>
          &createTransformer,
      const AutomatonBuilder &buildInitialCA, bool isForward);

  // Convert LLVM Module to WPDS
  void buildWPDS(Module &m, wpds::WPDS<GenKillTransformer> &wpds,
                 const std::function<GenKillTransformer *(Instruction *)>
                     &createTransformer,
                 bool isForward);

  // Create a configuration automaton for the initial states
  void buildInitialAutomaton(Module &m, wpds::CA<GenKillTransformer> &ca,
                             const std::set<Value *> &initialFacts,
                             bool isForward);
  void buildSeedAutomatonForFunctions(
      wpds::CA<GenKillTransformer> &ca, const std::vector<Function *> &functions,
      const std::set<Value *> &initialFacts, bool useExitPoints);
  ::ref_ptr<GenKillTransformer> querySummaryAtSymbol(wpds::wpds_key_t symbol) const;
  std::set<Value *> queryFactsAtSymbol(wpds::wpds_key_t symbol) const;
  GenKillTransformer *buildUnknownCallSummary(CallBase *callInst, Module &m,
                                              bool isForward) const;

  // Map program elements to WPDS keys. These return the exact keys used in the
  // engine's internal WPDS encoding when such keys exist.
  wpds::wpds_key_t getKeyForFunction(Function *f);
  wpds::wpds_key_t getKeyForInstruction(Instruction *inst);
  wpds::wpds_key_t getKeyForBasicBlock(BasicBlock *bb);
  wpds::wpds_key_t getKeyForCallSite(CallBase *callInst);
  wpds::wpds_key_t getKeyForReturnSite(CallBase *callInst);

  // Extract dataflow results from the resulting automaton
  void extractResults(Module &m, wpds::CA<GenKillTransformer> &resultCA,
                      std::unique_ptr<mono::DataFlowResult> &result,
                      bool isForward);

  // Map program elements to WPDS keys and vice versa
  std::map<Function *, wpds::wpds_key_t> functionToKey;
  std::map<Function *, wpds::wpds_key_t> functionExitToKey;
  std::map<Instruction *, wpds::wpds_key_t> instToKey;
  std::map<Instruction *, wpds::wpds_key_t> instPrevKey;
  std::map<BasicBlock *, wpds::wpds_key_t> bbToKey;
  std::map<CallBase *, wpds::wpds_key_t> callReturnToKey;
  std::map<wpds::wpds_key_t, Instruction *> keyToInst;
  std::map<Instruction *, std::set<Value *>> localGenByInst;
  std::map<Instruction *, std::set<Value *>> localKillByInst;

  // Maintain the dataflow result for the most recent analysis
  std::unique_ptr<mono::DataFlowResult> currentResult;
  std::unique_ptr<wpds::CA<GenKillTransformer>> lastResultCA;
  Query lastQuery = Query::user();
  llvm::Optional<wpds::wpds_key_t> lastAcceptState;
  CalleeResolver calleeResolver;
  ExternalCallPolicy externalCallPolicy;

  // Single WPDS control state shared by rules and the initial automaton
  wpds::wpds_key_t controlState;
};

} // namespace wpds

#endif // ANALYSIS_DATAFLOW_WPDS_INTERPROCEDURALDATAFLOWENGINE_H_
