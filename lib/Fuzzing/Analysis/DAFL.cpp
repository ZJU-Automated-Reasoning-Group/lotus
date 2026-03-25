// DAFL analysis using DyckVFG for slicing-based distance computation
// USENIX Security 23: DAFL: Directed Grey-box Fuzzing guided by Data Dependency
#include "Fuzzing/Analysis/DAFL.h"
#include "Fuzzing/Analysis/TargetDetection.h"

#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckModRefAnalysis.h"
#include "Alias/DyckAA/DyckVFG.h"

#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/VirtualFileSystem.h>

#include <limits>
#include <map>
#include <queue>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

AnalysisKey DAFLAnalysis::Key;

DAFLAnalysis::Result
DAFLAnalysis::readFromFile(Module &M, std::unique_ptr<MemoryBuffer> &Buffer) {
  SmallVector<StringRef, 0> AllLines;
  Buffer->getBuffer().split(AllLines, '\n');

  // The input file may contain multiple scores for the same file:line. This can
  // happen for example with ternary operators written in a single line. It
  // could also happen when linking multiple programs in parallel against the
  // same target library (i.e. specified target locations are in the library);
  // in this case make sure to use a unique DAFL file for each linker
  // invocation.
  // To  handle the general case we sum the scores for all file:line targets
  // instead of taking the max score over all file:line targets.
  StringMap<int> Scores;
  for (auto &Line : AllLines) {
    auto Trimmed = Line.trim();
    if (Trimmed.empty()) {
      continue;
    }

    // We expect a score followed by a file name, colon, line number.
    // For example: "42,foo.c:10"
    auto Parts = Trimmed.split(',');
    auto Score = Parts.first;
    auto FileLine = Parts.second;

    // If we didn't have a comma, then we are reading the old DAFL format.
    // In the old format, the file:line was first, then a colon, then the
    // score. For example: "foo.c:10:42"
    auto UseNewFormat = !Score.empty() && !FileLine.empty();
    if (!UseNewFormat) {
      auto Parts2 = Trimmed.rsplit(':');
      FileLine = Parts2.first;
      Score = Parts2.second;
    }

    int ScoreInt;
    if (Score.getAsInteger(10, ScoreInt)) {
      report_fatal_error("Invalid score in DAFL file");
    }

    if (!Scores.count(FileLine)) {
      Scores[FileLine] = ScoreInt;
    } else {
      Scores[FileLine] += ScoreInt;
    }
  }

  if (Scores.empty()) {
    report_fatal_error("No scores found in DAFL file");
  }

  StringMap<int> AggregatedScores;

  // Aggregate scores at the basic block level.
  // This is a map from file:line to the sum of all scores in the DAFL file for
  // that file:line.
  for (auto &Entry : Scores) {
    auto FileLine = Entry.first();
    auto Score = Entry.second;
    AggregatedScores[FileLine] = Score;
  }

  int MaxScore = 0;
  for (auto &KV : AggregatedScores) {
    if (KV.second > MaxScore) {
      MaxScore = KV.second;
    }
  }

  Result Res = Result::value_type();

  for (auto &F : M) {
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto *DL = I.getDebugLoc().get()) {
          auto Line = DL->getLine();
          auto File = DL->getFilename();
          auto FileLine = formatv("{0}:{1}", File, Line).str();
          if (AggregatedScores.count(FileLine)) {
            auto Score = AggregatedScores[FileLine];
            // Invert score so that lower distance = higher score
            auto Distance = static_cast<DAFLAnalysis::WeightTy>(MaxScore - Score + 1);
            if (Res->find(&BB) == Res->end()) {
              Res->insert({&BB, Distance});
            } else {
              // Take the minimum distance if we have multiple
              (*Res)[&BB] = std::min(Distance, (*Res)[&BB]);
            }
          }
        }
      }
    }
  }

  return Res;
}

DAFLAnalysis::Result DAFLAnalysis::run(Module &M, ModuleAnalysisManager &MAM) {
  if (!InputFile.empty()) {
    auto VFS = vfs::getRealFileSystem();
    auto BufferOrErr = VFS->getBufferForFile(InputFile);
    if (auto EC = BufferOrErr.getError()) {
      auto ErrorMessage = formatv("can't open DAFL input file '{0}': {1}",
                                  InputFile, EC.message());
      report_fatal_error(ErrorMessage);
    }

    errs() << "[DAFL] input file: " << InputFile << '\n';
    return readFromFile(M, *BufferOrErr);
  }

  // Get the target instructions
  SetVector<const Instruction *> TargetIs;
  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  for (auto &F : M) {
    auto &FTargets = FAM.getResult<AFLGoTargetDetectionAnalysis>(F);
    TargetIs.insert(FTargets.Is.begin(), FTargets.Is.end());
  }

  if (TargetIs.empty()) {
    if (NoTargetsNoError) {
      return {};
    }
    report_fatal_error("No target instructions found from target detection");
  }

  if (Verbose) {
    errs() << "[DAFL] Found " << TargetIs.size() << " target instructions\n";
    errs() << "[DAFL] Building Value Flow Graph using DyckVFG...\n";
  }

  // Run DyckAA and DyckModRef using legacy pass manager
  legacy::PassManager PM;
  auto *DyckAAPass = new DyckAliasAnalysis();
  auto *DyckMRAPass = new DyckModRefAnalysis();
  PM.add(DyckAAPass);
  PM.add(DyckMRAPass);
  PM.run(M);

  // Build the Value Flow Graph
  DyckVFG VFG(DyckAAPass, DyckMRAPass, &M);

  if (Verbose) {
    errs() << "[DAFL] VFG constructed, filtering target instructions...\n";
  }

  // Filter target instructions: remove intrinsics and their operands
  SetVector<const Instruction *> TargetsToRemove;
  for (auto I = TargetIs.rbegin(), E = TargetIs.rend(); I != E; ++I) {
    if (const auto *II = dyn_cast<IntrinsicInst>(*I)) {
      auto IID = II->getIntrinsicID();
      if (IID == Intrinsic::lifetime_start || IID == Intrinsic::lifetime_end ||
          IID == Intrinsic::dbg_declare || IID == Intrinsic::dbg_value ||
          IID == Intrinsic::dbg_label || IID == Intrinsic::dbg_addr) {
        TargetsToRemove.insert(II);
        // Add arguments to the intrinsic instruction
        for (const auto &Op : II->operands()) {
          if (const auto *OpI = dyn_cast<Instruction>(Op)) {
            if (!TargetIs.count(OpI)) {
              continue;
            }
            // Check that we don't remove instructions used by other targets
            bool HasNoOtherUser = true;
            for (const auto *U : OpI->users()) {
              if (U == II) {
                continue;
              }
              if (const auto *UI = dyn_cast<Instruction>(U)) {
                if (TargetIs.count(UI)) {
                  HasNoOtherUser = false;
                  break;
                }
              }
            }
            if (HasNoOtherUser) {
              TargetsToRemove.insert(OpI);
            }
          }
        }
      }
    }
  }

  TargetIs.set_subtract(TargetsToRemove);

  // Expand call instructions, return instructions, and conditional branches
  SetVector<const Instruction *> Worklist(TargetIs.begin(), TargetIs.end());
  TargetIs.clear();
  while (!Worklist.empty()) {
    auto *I = Worklist.pop_back_val();
    if (const auto *CB = dyn_cast<CallBase>(I)) {
      TargetsToRemove.insert(I);
      for (const auto &Arg : CB->args()) {
        if (const auto *ArgI = dyn_cast<Instruction>(Arg)) {
          Worklist.insert(ArgI);
        }
      }
    } else if (const auto *RI = dyn_cast<ReturnInst>(I)) {
      TargetsToRemove.insert(I);
      if (const auto *RV = RI->getReturnValue()) {
        if (const auto *RVI = dyn_cast<Instruction>(RV)) {
          Worklist.insert(RVI);
        }
      }
    } else if (const auto *BI = dyn_cast<BranchInst>(I)) {
      if (BI->isConditional()) {
        if (const auto *CI = dyn_cast<Instruction>(BI->getCondition())) {
          Worklist.insert(CI);
        }
      }
    } else {
      TargetIs.insert(I);
    }
  }

  // Skip remaining instructions that might not be in VFG
  TargetsToRemove.clear();
  for (const auto *I : TargetIs) {
    if (auto *BI = dyn_cast<BranchInst>(I)) {
      if (BI->isUnconditional()) {
        if (Verbose) {
          errs() << "[DAFL] Skipping unconditional branch target: " << *I << "\n";
        }
        TargetsToRemove.insert(I);
      }
    }
  }

  TargetIs.set_subtract(TargetsToRemove);

  if (TargetIs.empty()) {
    report_fatal_error("No target instructions left after filtering");
  }

  if (Verbose) {
    errs() << "[DAFL] After filtering: " << TargetIs.size() 
           << " target instructions\n";
    errs() << "[DAFL] Computing distances using backward slicing...\n";
  }

  // Track which instructions are present in VFG
  SmallSet<const Instruction *, 32> SeenTargetIs;

  // Build adjacency list from VFG (reversed for backward slicing)
  // Map from VFG node Value to its predecessors in the flow graph
  struct Edge {
    const Value *Target;
    const WeightTy Weight;
  };
  
  std::map<const Value *, std::vector<Edge>> G;
  const Value *SentinelNode = nullptr; // Use nullptr as sentinel

  // Build the graph from VFG
  for (auto NodeIt = VFG.node_begin(); NodeIt != VFG.node_end(); ++NodeIt) {
    auto *Node = *NodeIt;
    auto *NodeVal = Node->getValue();
    auto *NodeInst = dyn_cast_or_null<Instruction>(NodeVal);
    auto *NodeGEP = dyn_cast_or_null<GetElementPtrInst>(NodeVal);

    // Check if this node is a target instruction
    if (NodeInst) {
      for (auto *TargetI : TargetIs) {
        if (NodeInst == TargetI) {
          SeenTargetIs.insert(NodeInst);
          // Connect sentinel to target node
          G[SentinelNode].push_back({NodeVal, 0});
        }
      }
    }

    // Initialize adjacency list for this node
    if (G.find(NodeVal) == G.end()) {
      G[NodeVal] = std::vector<Edge>();
    }

    // Add edges from predecessors (incoming edges in VFG)
    for (auto InEdgeIt = Node->in_begin(); InEdgeIt != Node->in_end(); ++InEdgeIt) {
      auto *PredNode = InEdgeIt->first;
      auto *PredVal = PredNode->getValue();

      // Thin slicing: skip base pointer dereferences for GEP
      if (NodeGEP) {
        if (PredVal && PredVal == NodeGEP->getPointerOperand()) {
          continue;
        }
      }

      // Weight is 1 for actual instructions, 0 for non-instructions
      WeightTy Weight = 1;
      if (!NodeInst) {
        Weight = 0;
      }

      G[NodeVal].push_back({PredVal, Weight});
    }
  }

  // Check that all targets were found in VFG
  bool HasAllTargets = true;
  for (auto *TargetI : TargetIs) {
    if (SeenTargetIs.find(TargetI) == SeenTargetIs.end()) {
      HasAllTargets = false;
      errs() << "[DAFL] Warning: Target not found in VFG: " << *TargetI << "\n";
    }
  }

  if (!HasAllTargets && !NoTargetsNoError) {
    errs() << "[DAFL] Warning: Not all targets found in VFG, continuing anyway\n";
  }

  if (SeenTargetIs.empty()) {
    errs() << "[DAFL] Error: No target instructions found in VFG\n";
    if (NoTargetsNoError) {
      return {};
    }
    report_fatal_error("No target instructions found in VFG");
  }

  // Compute distances from sentinel node using Dijkstra's algorithm
  std::map<const Value *, WeightTy> Dist;
  std::map<const Value *, const Value *> Pred;
  auto UnreachableDist = std::numeric_limits<WeightTy>::max();
  
  for (auto &KV : G) {
    Dist[KV.first] = UnreachableDist;
    Pred[KV.first] = nullptr;
  }

  Dist[SentinelNode] = 0;
  
  // Priority queue: (distance, node)
  using QueueElem = std::pair<WeightTy, const Value *>;
  std::priority_queue<QueueElem, std::vector<QueueElem>, std::greater<QueueElem>> Q;
  Q.push({0, SentinelNode});

  while (!Q.empty()) {
    auto Top = Q.top();
    Q.pop();
    auto D = Top.first;
    auto *U = Top.second;

    // Skip if we've already processed this with a better distance
    if (D > Dist[U]) {
      continue;
    }

    // Process neighbors
    auto It = G.find(U);
    if (It != G.end()) {
      for (auto &Edge : It->second) {
        auto *V = Edge.Target;
        auto W = Edge.Weight;
        if (D + W < Dist[V]) {
          Dist[V] = D + W;
          Pred[V] = U;
          Q.push({Dist[V], V});
        }
      }
    }
  }

  // Find maximum distance
  WeightTy MaxDist = 0;
  for (auto &KV : Dist) {
    if (KV.first == SentinelNode || KV.second == UnreachableDist) {
      continue;
    }
    MaxDist = std::max(MaxDist, KV.second);
  }

  if (Verbose) {
    errs() << "[DAFL] Maximum distance from targets: " << MaxDist << "\n";
    errs() << "[DAFL] Aggregating scores by basic block...\n";
  }

  // Aggregate distances at basic block level
  Result Res = Result::value_type();
  for (auto &KV : Dist) {
    if (KV.first == SentinelNode || KV.second == UnreachableDist) {
      continue;
    }

    if (auto *I = dyn_cast_or_null<Instruction>(KV.first)) {
      // Score is proximity to target; higher is better
      auto Score = (MaxDist - KV.second) + 1;
      auto *BB = I->getParent();
      if (Res->find(BB) == Res->end()) {
        (*Res)[BB] = Score;
      } else {
        // Take maximum score for the basic block
        (*Res)[BB] = std::max(Score, (*Res)[BB]);
      }
    }
  }

  if (Verbose) {
    errs() << "[DAFL] Computed distances for " << Res->size() 
           << " basic blocks\n";
  }

  return Res;
}