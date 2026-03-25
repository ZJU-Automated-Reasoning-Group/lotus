#pragma once

#include "Alias/TPA/PointerAnalysis/FrontEnd/ConstPointerMap.h"

namespace llvm {
class Type;
} // namespace llvm

namespace tpa {

class TypeLayout;

using TypeMap = ConstPointerMap<llvm::Type, TypeLayout>;

} // namespace tpa