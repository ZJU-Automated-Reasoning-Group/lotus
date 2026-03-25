/******************************************************************************
 * Copyright (c) 2024 Fabian Schiebel.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Maximilian Leo Huber and others
 *****************************************************************************/

#include "Analysis/TypeHirarchy/LLVMVFTableData.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <sstream>

#include <spdlog/spdlog.h>

namespace lotus {

static LLVMVFTableData getDataFromJson(const std::string &JsonStr) {
  LLVMVFTableData Data;

  // Simple JSON parsing for VFT array
  // This is a simplified version - for full JSON support, consider using a
  // library
  std::istringstream iss(JsonStr);
  std::string line;
  bool inVFT = false;

  while (std::getline(iss, line)) {
    if (line.find("\"VFT\"") != std::string::npos) {
      inVFT = true;
      continue;
    }
    if (inVFT && line.find(']') != std::string::npos) {
      break;
    }
    if (inVFT) {
      size_t start = line.find('"');
      if (start != std::string::npos) {
        size_t end = line.find('"', start + 1);
        if (end != std::string::npos) {
          Data.VFT.push_back(line.substr(start + 1, end - start - 1));
        }
      }
    }
  }

  return Data;
}

void LLVMVFTableData::printAsJson(llvm::raw_ostream &OS) const {
  OS << "{\n  \"VFT\": [\n";
  for (size_t i = 0; i < VFT.size(); ++i) {
    OS << "    \"" << VFT[i] << "\"";
    if (i < VFT.size() - 1) {
      OS << ",";
    }
    OS << "\n";
  }
  OS << "  ]\n}\n";
}

LLVMVFTableData LLVMVFTableData::deserializeJson(const llvm::Twine &Path) {
  std::string PathStr = Path.str();

  std::ifstream file(PathStr);
  if (!file.is_open()) {
    SPDLOG_ERROR("Failed to open file: {}", PathStr);
    return LLVMVFTableData();
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return loadJsonString(buffer.str());
}

LLVMVFTableData LLVMVFTableData::loadJsonString(llvm::StringRef JsonAsString) {
  return getDataFromJson(JsonAsString.str());
}

} // namespace lotus
