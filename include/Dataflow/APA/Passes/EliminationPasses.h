#ifndef DATAFLOW_ELIMINATION_PASSES_ELIMINATIONPASSES_H_
#define DATAFLOW_ELIMINATION_PASSES_ELIMINATIONPASSES_H_

#include "llvm/Pass.h"

#include "Dataflow/APA/Clients/LLVM/Intra/AvailableExpressions.h"
#include "Dataflow/APA/Clients/LLVM/Intra/ConstantPropagation.h"
#include "Dataflow/APA/Clients/LLVM/Intra/LiveVariables.h"
#include "Dataflow/APA/Clients/LLVM/Intra/NonNull.h"
#include "Dataflow/APA/Clients/LLVM/Intra/Reachability.h"
#include "Dataflow/APA/Clients/LLVM/Intra/ReachingDefinitions.h"
#include "Dataflow/APA/Clients/LLVM/Intra/UninitializedVariables.h"
#include "Dataflow/APA/Clients/LLVM/Intra/VeryBusyExpressions.h"
#include "Dataflow/APA/Core/Options.h"

namespace elimination {

class ElimReachablePass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimReachablePass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override {
    return "Elimination Reachability";
  }

  const ReachableResult &getResult() const { return Result; }

private:
  ReachableResult Result;
};

class ElimConstantPropagationPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimConstantPropagationPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override {
    return "Elimination Constant Propagation";
  }

  const ConstantPropagationResult &getResult() const { return Result; }

private:
  ConstantPropagationResult Result;
};

class ElimReachingDefinitionsPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimReachingDefinitionsPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override {
    return "Elimination Reaching Definitions";
  }

  const ReachingDefinitionsResult &getResult() const { return Result; }

private:
  ReachingDefinitionsResult Result;
};

class ElimAvailableExpressionsPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimAvailableExpressionsPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override {
    return "Elimination Available Expressions";
  }

  const AvailableExpressionsResult &getResult() const { return Result; }

private:
  AvailableExpressionsResult Result;
};

class ElimUninitVariablesPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimUninitVariablesPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override {
    return "Elimination Uninitialized Variables";
  }

  const UninitVariablesResult &getResult() const { return Result; }

private:
  UninitVariablesResult Result;
};

class ElimLiveVariablesPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimLiveVariablesPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override {
    return "Elimination Live Variables";
  }

  const LiveVariablesResult &getResult() const { return Result; }

private:
  LiveVariablesResult Result;
};

class ElimVeryBusyExpressionsPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimVeryBusyExpressionsPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override {
    return "Elimination Very Busy Expressions";
  }

  const VeryBusyExpressionsResult &getResult() const { return Result; }

private:
  VeryBusyExpressionsResult Result;
};

class ElimNonNullPass final : public llvm::FunctionPass {
public:
  static char ID;
  ElimNonNullPass() : llvm::FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override { return "Elimination NonNull"; }

  const NonNullResult &getResult() const { return Result; }

private:
  NonNullResult Result;
};

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_PASSES_ELIMINATIONPASSES_H_
