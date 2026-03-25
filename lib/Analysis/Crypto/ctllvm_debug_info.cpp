/*
 * Debug-info helpers for the ctllvm pass.
 */

#include "CTInternal.h"

using namespace llvm;

namespace ctllvm {
namespace detail {
namespace {

template <typename T> struct DebugInfoExtractor;

template <> struct DebugInfoExtractor<StringRef> {
  template <typename DbgVarType>
  static StringRef extractName(DbgVarType *DbgVar) {
    return DbgVar->getName();
  }

  template <typename DbgType> static StringRef extractValue(DbgType *Dbg) {
    return Dbg->getVariable()->getName();
  }

  template <typename DbgDeclareType>
  static StringRef extractAddress(DbgDeclareType *DbgDeclare) {
    (void)DbgDeclare;
    return "";
  }

  static StringRef extractInstruction(Instruction *I) {
    (void)I;
    return "";
  }

  static StringRef extractLine(int line) {
    (void)line;
    return "";
  }

  static StringRef extractUnknown() { return "Unknown"; }
  static StringRef extractDefault() { return ""; }
};

template <> struct DebugInfoExtractor<Value *> {
  template <typename DbgVarType>
  static Value *extractName(DbgVarType *DbgVar) {
    (void)DbgVar;
    return nullptr;
  }

  template <typename DbgType> static Value *extractValue(DbgType *Dbg) {
    return Dbg->getValue();
  }

  template <typename DbgDeclareType>
  static Value *extractAddress(DbgDeclareType *DbgDeclare) {
    return DbgDeclare->getAddress();
  }

  static Value *extractInstruction(Instruction *I) { return I; }

  static Value *extractLine(int line) {
    (void)line;
    return nullptr;
  }

  static Value *extractUnknown() { return nullptr; }
  static Value *extractDefault() { return nullptr; }
};

template <> struct DebugInfoExtractor<int> {
  template <typename DbgVarType>
  static int extractName(DbgVarType *DbgVar) {
    (void)DbgVar;
    return -1;
  }

  template <typename DbgType> static int extractValue(DbgType *Dbg) {
    (void)Dbg;
    return -1;
  }

  template <typename DbgDeclareType>
  static int extractAddress(DbgDeclareType *DbgDeclare) {
    (void)DbgDeclare;
    return -1;
  }

  static int extractInstruction(Instruction *I) {
    (void)I;
    return -1;
  }

  static int extractLine(int line) { return line; }
  static int extractUnknown() { return -1; }
  static int extractDefault() { return -1; }
};

template <typename T>
T getDebugInfoImpl(Value *V, StringRef Name, Function &F) {
  for (auto &BB : F) {
    for (auto &I : BB) {
#if LLVM_VERSION_MAJOR >= 19
      for (DbgRecord &DR : I.getDbgRecordRange()) {
        if (auto *Dbg = dyn_cast<DbgVariableRecord>(&DR)) {
          auto *DbgVar = Dbg->getVariable();
          auto DbgLoc = DR.getDebugLoc();
          if ((V && Dbg->getValue() == V) ||
              (!Name.empty() && DbgVar->getName() == Name)) {
            if (std::is_same<T, StringRef>::value) {
              return DebugInfoExtractor<T>::extractValue(Dbg);
            }
            if (std::is_same<T, Value *>::value) {
              return DebugInfoExtractor<T>::extractValue(Dbg);
            }
            if (std::is_same<T, int>::value) {
              return DebugInfoExtractor<T>::extractLine(DbgLoc.getLine());
            }
          }
        }
      }
#endif
      if (auto *DbgDeclare = dyn_cast<DbgDeclareInst>(&I)) {
        if ((V && DbgDeclare->getAddress() == V) ||
            (!Name.empty() && DbgDeclare->getVariable()->getName() == Name)) {
          if (std::is_same<T, StringRef>::value) {
            return DebugInfoExtractor<T>::extractName(DbgDeclare->getVariable());
          }
          if (std::is_same<T, Value *>::value) {
            return DebugInfoExtractor<T>::extractAddress(DbgDeclare);
          }
          if (std::is_same<T, int>::value) {
            return DebugInfoExtractor<T>::extractLine(
                DbgDeclare->getDebugLoc().getLine());
          }
        }
      } else if (auto *DbgValue = dyn_cast<DbgValueInst>(&I)) {
        if ((V && DbgValue->getValue() == V) ||
            (!Name.empty() && DbgValue->getVariable()->getName() == Name)) {
          if (std::is_same<T, StringRef>::value) {
            return DebugInfoExtractor<T>::extractName(DbgValue->getVariable());
          }
          if (std::is_same<T, Value *>::value) {
            return DebugInfoExtractor<T>::extractValue(DbgValue);
          }
          if (std::is_same<T, int>::value) {
            const auto *DIVar =
                dyn_cast<DILocalVariable>(DbgValue->getVariable());
            if (DIVar) {
              return DebugInfoExtractor<T>::extractLine(DIVar->getLine());
            }
          }
        }
      } else if (I.hasMetadata() && I.getMetadata("dbg")) {
        if ((V && &I == V) || !Name.empty()) {
          auto DebugLoc = I.getDebugLoc();
          if (std::is_same<T, StringRef>::value) {
            return DebugInfoExtractor<T>::extractUnknown();
          }
          if (std::is_same<T, Value *>::value) {
            return DebugInfoExtractor<T>::extractInstruction(&I);
          }
          if (std::is_same<T, int>::value && DebugLoc) {
            return DebugInfoExtractor<T>::extractLine(DebugLoc.getLine());
          }
        }
      }
    }
  }

  return DebugInfoExtractor<T>::extractDefault();
}

} // namespace

StringRef getDebugName(Value *V, StringRef Name, Function &F) {
  return getDebugInfoImpl<StringRef>(V, Name, F);
}

Value *getDebugValue(Value *V, StringRef Name, Function &F) {
  return getDebugInfoImpl<Value *>(V, Name, F);
}

int getDebugLine(Value *V, StringRef Name, Function &F) {
  return getDebugInfoImpl<int>(V, Name, F);
}

} // namespace detail
} // namespace ctllvm
