#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

namespace OpenMP {

enum class DataSharingAttribute {
  None,
  Private,
  Shared,
  Firstprivate,
  Lastprivate,
  Copyin,
  Copyout,
  Linear,
  Reduction,
  SharedNoModify
};

struct DataSharingEntry {
  const llvm::Value *variable;
  DataSharingAttribute attribute;
  std::string clause;
  const llvm::Value *canonical_base = nullptr;
  int64_t offset = 0;
  bool has_precise_offset = false;
};

class DataSharingAnalysis {
public:
  explicit DataSharingAnalysis(llvm::Module &module);

  void analyze();

  bool isPrivate(const llvm::Value *v) const;
  bool isShared(const llvm::Value *v) const;
  bool isFirstprivate(const llvm::Value *v) const;

  DataSharingAttribute getAttribute(const llvm::Value *v) const;
  std::vector<DataSharingEntry>
  getEntriesForRegion(const llvm::Value *region) const;

private:
  llvm::Module &m_module;
  std::map<const llvm::Value *, DataSharingAttribute> m_variable_attributes;
  std::map<const llvm::Value *, std::vector<DataSharingEntry>> m_region_entries;
  std::vector<DataSharingEntry> m_entries;

  void scanGlobalAnnotations();
  void scanFunctionArguments(llvm::Function &func);
  void inferOutlinedFunctionCaptures(llvm::Function &func);
  const llvm::Value *canonicalizeValue(const llvm::Value *value) const;
  void addEntry(const llvm::Value *variable, DataSharingAttribute attribute,
                const std::string &clause, const llvm::Value *region = nullptr);
  DataSharingAttribute parseAttribute(const std::string &attr_str);
};
} // namespace OpenMP
