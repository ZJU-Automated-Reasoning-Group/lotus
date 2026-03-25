#include "Utils/Parallel/Scheduler/Task.h"

#include <sstream>

using namespace llvm;

std::string FunctionTask::toString() {
  std::ostringstream Oss;
  if (Func && Func->hasName()) {
    Oss << "FunctionTask[" << Func->getName().str() << "]";
  } else {
    Oss << "FunctionTask[anonymous]";
  }
  return Oss.str();
}

std::string SCCFunctionTask::toString() {
  std::ostringstream Oss;
  Oss << "SCCFunctionTask[" << Funcs.size() << " functions";
  if (!Funcs.empty() && Funcs.front() && Funcs.front()->hasName())
    Oss << ", first=" << Funcs.front()->getName().str();
  Oss << "]";
  return Oss.str();
}

std::string GCTask::toString() {
  std::ostringstream Oss;
  Oss << "GCTask[" << FuncSet.size() << " functions]";
  return Oss.str();
}
