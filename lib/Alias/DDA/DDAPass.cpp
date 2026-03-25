//===- DDAPass.cpp -- DDA driver (SVF-style) ------------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//

#include "Alias/DDA/DDAPass.h"

#include "Alias/DDA/ContextDDA.h"

#include <llvm/IR/Module.h>

using namespace lotus::analysis;
using namespace llvm;

DDAPass::~DDAPass() = default;

void DDAPass::selectClient(DDAClientKind k) {
  switch (k) {
  case DDAClientKind::All:
    client_ = std::make_unique<DDAClient>();
    client_->setSolveAll(true);
    break;
  case DDAClientKind::Funptr:
    client_ = std::make_unique<FunptrDDAClient>();
    break;
  case DDAClientKind::Alias:
    client_ = std::make_unique<AliasDDAClient>();
    break;
  }
}

void DDAPass::setClient(std::unique_ptr<DDAClient> client) {
  client_ = std::move(client);
}

void DDAPass::addQuery(const llvm::Value *v) {
  if (!client_)
    selectClient(DDAClientKind::All);
  if (client_)
    client_->addQuery(v);
}

bool DDAPass::mayAlias(const llvm::Value *v1, const llvm::Value *v2) const {
  if (kind_ == DDAKind::Cxt_DDA && contextDDA_)
    return contextDDA_->mayAlias(v1, v2);
  if (!flowDDA_)
    return true;
  return flowDDA_->mayAlias(v1, v2);
}

void DDAPass::runOnModule(Module &M) {
  if (!client_)
    selectClient(DDAClientKind::All);
  if (!client_)
    return;
  // Build FlowDDA first in all modes because ContextDDA reuses the same SVFG,
  // recursion metadata, and helper queries through FlowDDA.
  runPointerAnalysis(M, kind_);
}

void DDAPass::runPointerAnalysis(Module &M, DDAKind k) {
  ContextDDA::setMaxPathLen(this->getMaxPathLen());
  ContextDDA::setMaxCxtLen(this->getMaxContextLen());
  FlowDDA::setDefaultMaxBudget(maxBudget_);
  flowDDA_ = std::make_unique<FlowDDA>();
  if (!flowDDA_->run(M))
    return;
  flowDDA_->setClient(client_.get());
  switch (k) {
  case DDAKind::FlowS_DDA:
    // Pure flow-sensitive demand solving.
    flowDDA_->answerQueries();
    break;
  case DDAKind::Cxt_DDA:
    // Context-sensitive layer on top of the same flow-sensitive SVFG.
    contextDDA_ = std::make_unique<ContextDDA>(flowDDA_.get(), client_.get());
    contextDDA_->setInsensitiveRecursion(insensitiveRecursion_);
    contextDDA_->setInsensitiveCycle(insensitiveCycle_);
    contextDDA_->run(M);
    contextDDA_->answerQueries();
    break;
  }
}
