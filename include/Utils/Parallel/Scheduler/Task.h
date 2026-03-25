#ifndef LLVMUTILS_SCHEDULER_TASK_H
#define LLVMUTILS_SCHEDULER_TASK_H

#include <functional>
#include <memory>
#include <set>
#include <string>

#include <llvm/IR/Function.h>
#include <llvm/Support/Casting.h>

using namespace llvm;

/**
 * Task is the basic unit for parallel scheduler to schedule.
 * It consists of a work function and metadata for scheduling.
 */
class Task {
public:
  enum TaskKind {
    TK_Function, // Function-level analysis task
    TK_SCC,      // SCC-level analysis task
    TK_GC,       // Garbage collection task
    TK_Custom,   // Custom task
  };

protected:
  /// The weight of the task, which is used for prioritized task scheduling.
  unsigned int Weight;

  /// The kind of task
  TaskKind Kind;

public:
  Task(TaskKind TK) : Weight(0), Kind(TK) {}

  virtual ~Task() {}

  /// Execute the task
  virtual void run() = 0;

  unsigned int getWeight() const { return Weight; }

  void setWeight(unsigned int W) { Weight = W; }

  TaskKind getTaskKind() const { return Kind; }

  /// Debug string representation
  virtual std::string toString() = 0;
};

/**
 * FunctionTask represents a task that operates on a single function.
 * The client provides a callback that will be invoked with the function.
 */
class FunctionTask : public Task {
private:
  const Function *Func;
  std::function<void(const Function *)> Callback;
  void *Context; // Opaque context pointer for the client

public:
  FunctionTask(const Function *F, std::function<void(const Function *)> CB,
               void *Ctx = nullptr)
      : Task(TK_Function), Func(F), Callback(CB), Context(Ctx) {}

  virtual void run() override { Callback(Func); }

  const Function *getFunction() const { return Func; }

  void *getContext() const { return Context; }

  virtual std::string toString() override;

public:
  static bool classof(const Task *T) { return T->getTaskKind() == TK_Function; }
};

/**
 * SCCFunctionTask represents an SCC-level unit of work.
 * Functions inside the SCC are executed serially in a deterministic order.
 */
class SCCFunctionTask : public Task {
private:
  int SCCIndex;
  std::vector<const Function *> Funcs;
  std::function<void(const Function *)> Callback;
  void *Context;

public:
  SCCFunctionTask(int SCCIdx, std::vector<const Function *> Functions,
                  std::function<void(const Function *)> CB, void *Ctx = nullptr)
      : Task(TK_SCC), SCCIndex(SCCIdx), Funcs(std::move(Functions)),
        Callback(CB), Context(Ctx) {}

  virtual void run() override {
    for (const Function *F : Funcs)
      Callback(F);
  }

  int getSCCIndex() const { return SCCIndex; }

  const std::vector<const Function *> &getFunctions() const { return Funcs; }

  void *getContext() const { return Context; }

  virtual std::string toString() override;

public:
  static bool classof(const Task *T) { return T->getTaskKind() == TK_SCC; }
};

/**
 * GCTask performs garbage collection / memory cleanup.
 * The client provides a callback for each function to be released.
 */
class GCTask : public Task {
private:
  std::set<const Function *> FuncSet;
  std::function<void(const Function *)> ReleaseCallback;
  void *Context;

public:
  GCTask(const std::set<const Function *> &Funcs,
         std::function<void(const Function *)> CB, void *Ctx = nullptr)
      : Task(TK_GC), FuncSet(Funcs), ReleaseCallback(CB), Context(Ctx) {}

  virtual void run() override {
    for (auto *F : FuncSet) {
      ReleaseCallback(F);
    }
  }

  void *getContext() const { return Context; }

  virtual std::string toString() override;

public:
  static bool classof(const Task *T) { return T->getTaskKind() == TK_GC; }
};

/**
 * CustomTask allows arbitrary work to be scheduled.
 */
class CustomTask : public Task {
private:
  std::function<void()> Callback;
  std::string Name;

public:
  CustomTask(std::function<void()> CB, const std::string &Name = "CustomTask")
      : Task(TK_Custom), Callback(CB), Name(Name) {}

  virtual void run() override { Callback(); }

  virtual std::string toString() override { return Name; }

public:
  static bool classof(const Task *T) { return T->getTaskKind() == TK_Custom; }
};

#endif // LLVMUTILS_SCHEDULER_TASK_H
