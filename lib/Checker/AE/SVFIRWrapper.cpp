//===- SVFIRWrapper.cpp -- SVFIR-like interface using AserPTA ----------//
//
// Implementation of SVFIRWrapper using AserPTA
//
//===----------------------------------------------------------------------===//

#include "Checker/AE/SVFIRWrapper.h"

#include "Alias/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/AserPTA/PointerAnalysis/Graph/CallGraph.h"
#include "Alias/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSObject.h"
#include "Alias/AserPTA/PointerAnalysis/PointerAnalysisPass.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/PointsTo/BitVectorPTS.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/WavePropagation.h"

namespace lotus {
namespace analysis {

using PTASolver = aser::WavePropagation<aser::DefaultLangModel<
    aser::NoCtx, aser::FSMemModel<aser::NoCtx>, aser::BitVectorPTS>>;
using PTAPass = PointerAnalysisPass<PTASolver>;
using FSObject = aser::FSObject<aser::NoCtx>;

SVFIRWrapper::SVFIRWrapper(void *ptaSolver, llvm::Module *module)
    : ptaSolver_(ptaSolver), module_(module) {}

SVFIRWrapper::~SVFIRWrapper() = default;

void SVFIRWrapper::getPointsTo(const llvm::Value *V,
                               std::vector<void *> &result) const {
  result.clear();
  if (!isPTAReady() || !V || !V->getType()->isPointerTy())
    return;

  auto *pta = static_cast<PTAPass *>(ptaSolver_);
  if (!pta)
    return;

  auto *solver = pta->getPTA();
  if (!solver)
    return;

  // Get points-to objects using the solver
  std::vector<const FSObject *> pts;
  solver->getPointsTo(nullptr, V, pts);

  for (const auto *obj : pts) {
    result.push_back(const_cast<FSObject *>(obj));
  }
}

const llvm::Type *SVFIRWrapper::getObjectType(const llvm::Value *V) const {
  if (!isPTAReady() || !V || !V->getType()->isPointerTy())
    return nullptr;

  auto *pta = static_cast<PTAPass *>(ptaSolver_);
  if (!pta)
    return nullptr;

  auto *solver = pta->getPTA();
  if (!solver)
    return nullptr;

  return solver->getPointedType(nullptr, V);
}

bool SVFIRWrapper::alias(const llvm::Value *v1, const llvm::Value *v2) const {
  if (!isPTAReady() || !v1 || !v2)
    return false;

  auto *pta = static_cast<PTAPass *>(ptaSolver_);
  if (!pta)
    return false;

  auto *solver = pta->getPTA();
  if (!solver)
    return false;

  return solver->alias(nullptr, v1, nullptr, v2);
}

const llvm::Function *SVFIRWrapper::getFunction(const std::string &name) const {
  if (module_)
    return module_->getFunction(name);
  return nullptr;
}

uint32_t SVFIRWrapper::getByteSizeOfObj(const void *obj) const {
  if (!obj || !module_)
    return 0;

  const auto *fsObj = static_cast<const FSObject *>(obj);
  if (!fsObj)
    return 0;

  // Get the allocation site value and compute size from its type
  const llvm::Value *allocSite = fsObj->getAllocSite().getValue();
  if (!allocSite)
    return 0;

  const llvm::DataLayout &dl = module_->getDataLayout();

  // For allocations (alloca, malloc, etc.), use the allocated type
  if (const auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(allocSite)) {
    uint64_t elemSize = dl.getTypeAllocSize(alloca->getAllocatedType());
    if (!alloca->isArrayAllocation()) {
      return static_cast<uint32_t>(elemSize);
    }

    if (const auto *arraySize =
            llvm::dyn_cast<llvm::ConstantInt>(alloca->getArraySize())) {
      return static_cast<uint32_t>(elemSize * arraySize->getZExtValue());
    }

    return 0;
  }

  // For global variables, use the value type
  if (const auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(allocSite)) {
    return dl.getTypeAllocSize(gv->getValueType());
  }

  // For pointer types, get the pointee type
  if (allocSite->getType()->isPointerTy()) {
    llvm::Type *pointeeType = allocSite->getType()->getPointerElementType();
    if (pointeeType && pointeeType->isSized()) {
      return dl.getTypeAllocSize(pointeeType);
    }
  }

  return 0;
}

const llvm::Value *SVFIRWrapper::getObjValue(const void *obj) const {
  if (!obj)
    return nullptr;

  const auto *fsObj = static_cast<const FSObject *>(obj);
  if (!fsObj)
    return nullptr;

  return fsObj->getValue();
}

} // namespace analysis
} // namespace lotus
