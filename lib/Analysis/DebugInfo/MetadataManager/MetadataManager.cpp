/**
 * @file MetadataManager.cpp
 * @brief Metadata Management Implementation
 *
 * This file implements the MetadataManager class for managing user-defined
 * metadata attached to LLVM IR elements including modules, loops, instructions,
 * functions, and variables.
 *
 * @author Lotus Analysis Framework
 * @date 2025
 * @ingroup DebugInfo
 */

/*
 * Copyright 2021 - 2022  Simone Campanoni
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do
 so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "Analysis/DebugInfo/MetadataManager/MetadataManager.h"

namespace noelle {

/**
 * @brief Construct a MetadataManager for a module
 * @param M The LLVM module to manage metadata for
 *
 * Initializes metadata tracking by collecting:
 * - Variable annotations from llvm.var.annotation calls
 * - Function annotations from llvm.global.annotations
 * - Global variable annotations
 */
MetadataManager::MetadataManager(Module &M) : program{M} {

  /*
   * Collect variable metadata from llvm.var.annotation (pre-LLVM 14 style)
   * and llvm.ptr.annotation (LLVM 14+ style).
   *
   * Both intrinsics have the annotation string as their second operand,
   * encoded as a GEP into a global constant string.
   *
   * We also strip all pointer casts (BitCastInst, AddrSpaceCastInst) when
   * resolving the annotated variable, not just a single BitCastInst.
   */
  auto extractAnnotationString = [](CallInst *call,
                                    unsigned annotOperandIdx) -> std::string {
    if (annotOperandIdx >= call->arg_size()) {
      return "";
    }
    // The annotation operand may be a GEP or a direct GlobalVariable.
    Value *annotOp = call->getArgOperand(annotOperandIdx);
    GlobalVariable *annoteStr = nullptr;
    if (auto *gep = dyn_cast<GetElementPtrInst>(annotOp)) {
      annoteStr = dyn_cast<GlobalVariable>(gep->getOperand(0));
    } else if (auto *ce = dyn_cast<ConstantExpr>(annotOp)) {
      // Constant GEP folded at compile time
      if (ce->getOpcode() == Instruction::GetElementPtr) {
        annoteStr = dyn_cast<GlobalVariable>(ce->getOperand(0));
      }
    } else {
      annoteStr = dyn_cast<GlobalVariable>(annotOp);
    }
    if (!annoteStr) {
      return "";
    }
    auto *data = dyn_cast<ConstantDataSequential>(annoteStr->getInitializer());
    if (!data || !data->isString()) {
      return "";
    }
    return data->getAsString().str();
  };

  // Strip all pointer casts to reach the underlying alloca.
  auto stripCastsToAlloca = [](Value *V) -> AllocaInst * {
    while (V) {
      if (auto *AI = dyn_cast<AllocaInst>(V)) {
        return AI;
      }
      if (auto *BC = dyn_cast<BitCastInst>(V)) {
        V = BC->getOperand(0);
      } else if (auto *AC = dyn_cast<AddrSpaceCastInst>(V)) {
        V = AC->getOperand(0);
      } else {
        break;
      }
    }
    return nullptr;
  };

  for (auto &F : M) {
    for (auto &inst : instructions(F)) {
      auto *call = dyn_cast<CallInst>(&inst);
      if (call == nullptr) {
        continue;
      }
      auto *callee = call->getCalledFunction();
      if (callee == nullptr) {
        continue;
      }

      StringRef calleeName = callee->getName();

      // Handle llvm.var.annotation (operand 0 = ptr, operand 1 = annotation)
      if (calleeName == "llvm.var.annotation") {
        auto *var = stripCastsToAlloca(call->getArgOperand(0));
        if (!var) {
          continue;
        }
        std::string annot = extractAnnotationString(call, 1);
        if (!annot.empty()) {
          this->varMetadata[var].insert(annot);
        }
        continue;
      }

      // Handle llvm.ptr.annotation.* (LLVM 14+ replacement for var.annotation)
      // Signature: i8* @llvm.ptr.annotation.p0i8(i8* %ptr, i8* %annot, ...)
      if (calleeName.startswith("llvm.ptr.annotation")) {
        // The first operand is the pointer being annotated; strip casts.
        auto *var = stripCastsToAlloca(call->getArgOperand(0));
        if (!var) {
          continue;
        }
        std::string annot = extractAnnotationString(call, 1);
        if (!annot.empty()) {
          this->varMetadata[var].insert(annot);
        }
        continue;
      }
    }
  }

  /*
   * Collect metadata attached to functions.
   */
  auto *globalArray = M.getGlobalVariable("llvm.global.annotations");
  if (globalArray != nullptr) {
    for (auto &globalArrayEntry : globalArray->operands()) {
      auto *globalArrayEntryConstant =
          dyn_cast<ConstantArray>(globalArrayEntry);
      if (globalArrayEntryConstant == nullptr) {
        continue;
      }
      for (auto &globalArrayEntryOperand :
           globalArrayEntryConstant->operands()) {

        /*
         * Fetch the annotation.
         */
        auto *globalArrayEntryOperandStruct =
            dyn_cast<ConstantStruct>(globalArrayEntryOperand);
        if (globalArrayEntryOperandStruct == nullptr) {
          continue;
        }
        if (globalArrayEntryOperandStruct->getNumOperands() < 2) {
          continue;
        }
        auto *annotationVariable = dyn_cast<GlobalVariable>(
            globalArrayEntryOperandStruct->getOperand(1)->getOperand(0));
        if (annotationVariable == nullptr) {
          continue;
        }
        auto *A =
            dyn_cast<ConstantDataArray>(annotationVariable->getOperand(0));
        if (A == nullptr) {
          continue;
        }
        auto AS = A->getAsString();

        /*
         * Attach the annotation.
         *
         * Case 0: function
         */
        auto *annotatedFunction = dyn_cast<Function>(
            globalArrayEntryOperandStruct->getOperand(0)->getOperand(0));
        if (annotatedFunction != nullptr) {
          this->functionMetadata[annotatedFunction].insert(AS.str());
          continue;
        }

        /*
         * Case 1: global
         */
        auto *annotatedGlobal = dyn_cast<GlobalVariable>(
            globalArrayEntryOperandStruct->getOperand(0)->getOperand(0));
        if (annotatedGlobal != nullptr) {
          this->globalMetadata[annotatedGlobal].insert(AS.str());
          continue;
        }
      }
    }
  }

  return;
}

bool MetadataManager::doesHaveMetadata(LoopStructure *loop,
                                       const std::string &metadataName) {

  /*
   * Check if we have already cached the metadata.
   */
  auto loopEntriesIt = this->metadata.find(loop);
  if (loopEntriesIt != this->metadata.end()) {
    auto *const loopEntriesPair = &*loopEntriesIt;
    const auto &loopEntries = loopEntriesPair->second;
    if (loopEntries.find(metadataName) != loopEntries.end()) {

      /*
       * We have already cached the metadata.
       */
      return true;
    }
  }

  /*
   * We did not have cached the metadata.
   * Check the IR.
   *
   * Fetch the header terminator.
   */
  auto *headerTerm = loop->getHeader()->getTerminator();

  /*
   * Check if the metadata exists.
   */
  auto *metaNode = headerTerm->getMetadata(metadataName);
  if (!metaNode) {
    return false;
  }

  /*
   * Cache the metadata since it exists.
   */
  auto metaString = cast<MDString>(metaNode->getOperand(0))->getString();
  this->metadata[loop][metadataName] =
      new MetadataEntry(metadataName, metaString.str());

  return true;
}

std::string MetadataManager::getMetadata(LoopStructure *loop,
                                         const std::string &metadataName) {

  /*
   * Check if the metadata exists.
   */
  if (!this->doesHaveMetadata(loop, metadataName)) {
    return "";
  }

  auto loopEntries = this->metadata.at(loop);
  auto *metadataEntry = loopEntries.at(metadataName);
  return metadataEntry->getValue();
}

void MetadataManager::setMetadata(LoopStructure *loop,
                                  const std::string &metadataName,
                                  const std::string &metadataValue) {

  /*
   * Fetch the header terminator.
   */
  auto *headerTerm = loop->getHeader()->getTerminator();

  /*
   * Check if the metadata node already exists.
   */
  auto *metaNode = headerTerm->getMetadata(metadataName);
  if (!metaNode) {
    errs() << "MetadataManager::setMetadata: ERROR = the metadata \""
           << metadataName << "\" does not exists in the loop " << *headerTerm
           << "\n";
    abort();
  }

  /*
   * Set the metadata
   */
  auto &cxt = headerTerm->getContext();
  auto *s = MDString::get(cxt, metadataValue);
  auto *n = MDNode::get(cxt, s);
  headerTerm->setMetadata(metadataName, n);

  /*
   * Add the metadata to our mapping.
   */
  this->addMetadata(loop, metadataName);

  return;
}

void MetadataManager::deleteMetadata(LoopStructure *loop,
                                     const std::string &metadataName) {

  /*
   * Fetch the header terminator.
   */
  auto *headerTerm = loop->getHeader()->getTerminator();

  /*
   * Check if the metadata node already exists.
   */
  auto *metaNode = headerTerm->getMetadata(metadataName);
  if (!metaNode) {
    errs() << "MetadataManager::deleteMetadata: ERROR = the metadata \""
           << metadataName << "\" does not exists in the loop " << *headerTerm
           << "\n";
    abort();
  }

  /*
   * Delete the metadata from the IR.
   */
  headerTerm->setMetadata(metadataName, nullptr);

  /*
   * Remove the metadata from our mapping (use reference, not copy).
   */
  auto loopIt = this->metadata.find(loop);
  if (loopIt != this->metadata.end()) {
    auto entryIt = loopIt->second.find(metadataName);
    if (entryIt != loopIt->second.end()) {
      delete entryIt->second;
      loopIt->second.erase(entryIt);
    }
  }

  return;
}

void MetadataManager::addMetadata(LoopStructure *loop,
                                  const std::string &metadataName,
                                  const std::string &metadataValue) {

  /*
   * Fetch the header terminator.
   */
  auto *headerTerm = loop->getHeader()->getTerminator();

  /*
   * Check if the metadata node already exists.
   */
  auto *metaNode = headerTerm->getMetadata(metadataName);
  if (metaNode) {
    errs() << "MetadataManager::addMetadata: ERROR = the metadata \""
           << metadataName << "\" already exists in the loop " << *headerTerm
           << "\n";
    abort();
  }

  /*
   * Create the metadata and add it to the IR.
   */
  auto &cxt = headerTerm->getContext();
  auto *s = MDString::get(cxt, metadataValue);
  auto *n = MDNode::get(cxt, s);
  headerTerm->setMetadata(metadataName, n);

  /*
   * Add the metadata to our mapping.
   */
  this->addMetadata(loop, metadataName);

  return;
}

void MetadataManager::addMetadata(LoopStructure *loop,
                                  const std::string &metadataName) {

  /*
   * Fetch the header terminator.
   */
  auto *headerTerm = loop->getHeader()->getTerminator();

  /*
   * Fetch the metadata node.
   */
  auto *metaNode = headerTerm->getMetadata(metadataName);
  if (!metaNode) {
    return;
  }

  /*
   * Fetch the string.
   */
  auto metaString = cast<MDString>(metaNode->getOperand(0))->getString();

  /*
   * Add the metadata to the actual map (use reference, not a copy).
   * If an entry already exists for this key, delete the old one first to
   * avoid a memory leak.
   */
  auto &loopEntries = this->metadata[loop];
  auto it = loopEntries.find(metadataName);
  if (it != loopEntries.end()) {
    delete it->second;
    it->second = new MetadataEntry(metadataName, metaString.str());
  } else {
    loopEntries[metadataName] =
        new MetadataEntry(metadataName, metaString.str());
  }

  return;
}

} // namespace noelle