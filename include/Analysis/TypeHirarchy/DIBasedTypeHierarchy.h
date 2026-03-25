/******************************************************************************
 * Copyright (c) 2023 Fabian Schiebel.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Fabian Schiebel and others
 *****************************************************************************/

#ifndef LOTUS_ANALYSIS_TYPEHIERARCHY_DIBASEDTYPEHIERARCHY_H
#define LOTUS_ANALYSIS_TYPEHIERARCHY_DIBASEDTYPEHIERARCHY_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/Casting.h"

#include "Analysis/TypeHirarchy/DIBasedTypeHierarchyData.h"
#include "Analysis/TypeHirarchy/LLVMVFTable.h"
#include "Analysis/TypeHirarchy/TypeHierarchy.h"

#include <deque>

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {

/// \brief Represents the type hierarchy of the target program.
///
/// \note This class only works, if the target program's IR was generated with
/// debug information. Pass `-g` to the compiler to achieve this.
class DIBasedTypeHierarchy
    : public TypeHierarchy<const llvm::DIType *, const llvm::Function *> {
public:
  using ClassType = const llvm::DIType *;
  using f_t = const llvm::Function *;

  static constexpr const char *StructPrefix = "struct.";
  static constexpr const char *ClassPrefix = "class.";
  static constexpr const char *VTablePrefix = "_ZTV";
  static constexpr const char *VTablePrefixDemang = "vtable for ";
  static constexpr const char *PureVirtualCallName = "__cxa_pure_virtual";

  /// \brief Creates a type hierarchy based on an intermediate representation
  /// database.
  /// \param[in] M The LLVM Module of which the type hierarchy will be based
  /// upon. This MUST contain debug information for the algorithm to work!
  explicit DIBasedTypeHierarchy(const llvm::Module &M);

  /// \brief Loads an already computed type hierarchy.
  /// \param[in] M The LLVM Module of the type hierarchy.
  /// \param[in] SerializedData The already existing type hierarchy, given by
  /// the appropiate class DIBasedTypeHierarchyData, which contains all
  /// neccesary information.
  explicit DIBasedTypeHierarchy(const llvm::Module *M,
                                const DIBasedTypeHierarchyData &SerializedData);
  ~DIBasedTypeHierarchy() override = default;

  [[nodiscard]] bool hasType(ClassType Type) const override {
    return TypeToVertex.count(Type);
  }

  [[nodiscard]] bool isSubType(ClassType Type,
                               ClassType SubType) const override {
    return llvm::is_contained(subTypesOf(Type), SubType);
  }

  [[nodiscard]] std::set<ClassType> getSubTypes(ClassType Type) const override {
    const auto &Range = subTypesOf(Type);
    return {Range.begin(), Range.end()};
  }

  /// A more efficient version of getSubTypes()
  [[nodiscard]] llvm::iterator_range<const ClassType *>
  subTypesOf(ClassType Ty) const noexcept;

  [[nodiscard]] llvm::Optional<ClassType>
  getType(llvm::StringRef TypeName) const noexcept override {
    auto It = NameToType.find(TypeName);
    if (It != NameToType.end()) {
      return It->second;
    }
    return llvm::None;
  }

  [[nodiscard]] std::vector<ClassType> getAllTypes() const override {
    return {VertexTypes.begin(), VertexTypes.end()};
  }

  [[nodiscard]] const auto &getAllVTables() const noexcept { return VTables; }

  [[nodiscard]] static llvm::StringRef typeName(ClassType Type) {
    if (const auto *CompTy = llvm::dyn_cast<llvm::DICompositeType>(Type)) {
      auto Ident = CompTy->getIdentifier();
      return Ident.empty() ? CompTy->getName() : Ident;
    }
    return Type->getName();
  }

  [[nodiscard]] llvm::StringRef getTypeName(ClassType Type) const override {
    return typeName(Type);
  }

  [[nodiscard]] size_t size() const noexcept override {
    return VertexTypes.size();
  }
  [[nodiscard]] bool empty() const noexcept override {
    return VertexTypes.empty();
  }

  void print(llvm::raw_ostream &OS = llvm::outs()) const override;

  /// \brief Prints the class hierarchy to an ostream in dot format.
  /// \param OS outputstream
  void printAsDot(llvm::raw_ostream &OS = llvm::outs()) const;

  /// \brief Prints the class hierarchy to an ostream in JSON format.
  /// \param OS outputstream
  void printAsJson(llvm::raw_ostream &OS = llvm::outs()) const override;

private:
  [[nodiscard]] DIBasedTypeHierarchyData getTypeHierarchyData() const;
  [[nodiscard]] llvm::iterator_range<const ClassType *>
  subTypesOf(size_t TypeIdx) const noexcept;

  // ---

  llvm::StringMap<ClassType> NameToType;
  // Map each type to an integer index that is used by VertexTypes and
  // DerivedTypesOf.
  // Note: all the below arrays should always have the same size (except for
  // Hierarchy)!
  llvm::DenseMap<ClassType, size_t> TypeToVertex;
  // The class types we care about ("VertexProperties")
  std::vector<const llvm::DICompositeType *> VertexTypes;
  std::vector<std::pair<uint32_t, uint32_t>> TransitiveDerivedIndex;
  // The inheritance graph linearized as-if constructed by L2R pre-order
  // traversal from the roots. Allows efficient access to the transitive closure
  // without ever storing it explicitly. This only works, because the type-graph
  // is known to never contain loops
  std::vector<ClassType> Hierarchy;

  // The VTables of the polymorphic types in the TH. default-constructed if not
  // exists
  std::deque<LLVMVFTable> VTables;
};
} // namespace lotus

#endif // LOTUS_ANALYSIS_TYPEHIERARCHY_DIBASEDTYPEHIERARCHY_H
