/******************************************************************************
 * Copyright (c) 2024 Fabian Schiebel.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Maximilian Leo Huber and others
 *****************************************************************************/

#include "Analysis/TypeHirarchy/DIBasedTypeHierarchyData.h"

#include "llvm/Support/JSON.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <sstream>

#include <spdlog/spdlog.h>

namespace lotus {

static DIBasedTypeHierarchyData getDataFromJson(const std::string &JsonStr) {
  DIBasedTypeHierarchyData Data;
  auto Parsed = llvm::json::parse(JsonStr);
  if (!Parsed) {
    SPDLOG_ERROR("Failed to parse type hierarchy JSON: {}",
                 llvm::toString(Parsed.takeError()));
    return Data;
  }

  auto *Root = Parsed->getAsObject();
  if (!Root) {
    SPDLOG_ERROR("Type hierarchy JSON root is not an object");
    return Data;
  }

  if (auto *VertexTypes = Root->getArray("VertexTypes")) {
    Data.VertexTypes.reserve(VertexTypes->size());
    for (const auto &Entry : *VertexTypes) {
      if (auto Value = Entry.getAsString()) {
        Data.VertexTypes.push_back(std::string(*Value));
      }
    }
  }

  if (auto *TransitiveDerivedIndex = Root->getArray("TransitiveDerivedIndex")) {
    Data.TransitiveDerivedIndex.reserve(TransitiveDerivedIndex->size());
    for (const auto &Entry : *TransitiveDerivedIndex) {
      auto *Pair = Entry.getAsArray();
      if (!Pair || Pair->size() != 2) {
        continue;
      }
      auto First = (*Pair)[0].getAsInteger();
      auto Second = (*Pair)[1].getAsInteger();
      if (!First || !Second) {
        continue;
      }
      Data.TransitiveDerivedIndex.emplace_back(
          static_cast<uint32_t>(*First), static_cast<uint32_t>(*Second));
    }
  }

  if (auto *Hierarchy = Root->getArray("Hierarchy")) {
    Data.Hierarchy.reserve(Hierarchy->size());
    for (const auto &Entry : *Hierarchy) {
      if (auto Value = Entry.getAsString()) {
        Data.Hierarchy.push_back(std::string(*Value));
      }
    }
  }

  if (auto *VTables = Root->getArray("VTables")) {
    Data.VTables.reserve(VTables->size());
    for (const auto &Entry : *VTables) {
      auto *VTable = Entry.getAsArray();
      if (!VTable) {
        continue;
      }
      std::vector<std::string> Current;
      Current.reserve(VTable->size());
      for (const auto &Func : *VTable) {
        if (auto Value = Func.getAsString()) {
          Current.push_back(std::string(*Value));
        }
      }
      Data.VTables.emplace_back(std::move(Current));
    }
  }

  return Data;
}

void DIBasedTypeHierarchyData::printAsJson(llvm::raw_ostream &OS) {
  OS << "{\n";
  OS << "  \"VertexTypes\": [\n";
  for (size_t i = 0; i < VertexTypes.size(); ++i) {
    OS << "    \"" << VertexTypes[i] << "\"";
    if (i < VertexTypes.size() - 1)
      OS << ",";
    OS << "\n";
  }
  OS << "  ],\n";

  OS << "  \"TransitiveDerivedIndex\": [\n";
  for (size_t i = 0; i < TransitiveDerivedIndex.size(); ++i) {
    OS << "    [" << TransitiveDerivedIndex[i].first << ", "
       << TransitiveDerivedIndex[i].second << "]";
    if (i < TransitiveDerivedIndex.size() - 1)
      OS << ",";
    OS << "\n";
  }
  OS << "  ],\n";

  OS << "  \"Hierarchy\": [\n";
  for (size_t i = 0; i < Hierarchy.size(); ++i) {
    OS << "    \"" << Hierarchy[i] << "\"";
    if (i < Hierarchy.size() - 1)
      OS << ",";
    OS << "\n";
  }
  OS << "  ],\n";

  OS << "  \"VTables\": [\n";
  for (size_t i = 0; i < VTables.size(); ++i) {
    OS << "    [\n";
    for (size_t j = 0; j < VTables[i].size(); ++j) {
      OS << "      \"" << VTables[i][j] << "\"";
      if (j < VTables[i].size() - 1)
        OS << ",";
      OS << "\n";
    }
    OS << "    ]";
    if (i < VTables.size() - 1)
      OS << ",";
    OS << "\n";
  }
  OS << "  ]\n";
  OS << "}\n";
}

DIBasedTypeHierarchyData
DIBasedTypeHierarchyData::deserializeJson(const llvm::Twine &Path) {
  std::string PathStr = Path.str();

  std::ifstream file(PathStr);
  if (!file.is_open()) {
    SPDLOG_ERROR("Failed to open file: {}", PathStr);
    return DIBasedTypeHierarchyData();
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return loadJsonString(buffer.str());
}

DIBasedTypeHierarchyData
DIBasedTypeHierarchyData::loadJsonString(llvm::StringRef JsonAsString) {
  return getDataFromJson(JsonAsString.str());
}

} // namespace lotus
