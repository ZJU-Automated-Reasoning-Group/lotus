//===- AbsExtAPI.cpp -- Abstract Interpretation External API handler//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#include "Checker/AE/AbsExtAPI.h"

#include "Checker/AE/AbstractInterpretation.h"
#include "Checker/AE/AbstractState.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace analysis {

namespace {

int64_t clampFieldOffset(int64_t offset) {
  if (offset < -static_cast<int64_t>(MaxFieldLimit))
    return -static_cast<int64_t>(MaxFieldLimit);
  if (offset > static_cast<int64_t>(MaxFieldLimit))
    return static_cast<int64_t>(MaxFieldLimit);
  return offset;
}

int64_t clampFieldOffset(const BoundedInt &bound) {
  if (bound.is_minus_infinity())
    return -static_cast<int64_t>(MaxFieldLimit);
  if (bound.is_plus_infinity())
    return static_cast<int64_t>(MaxFieldLimit);
  return clampFieldOffset(bound.getIntNumeral());
}

bool containsAny(const std::string &name,
                 std::initializer_list<const char *> patterns) {
  for (const char *pattern : patterns) {
    if (name.find(pattern) != std::string::npos)
      return true;
  }
  return false;
}

enum class IntegerSignedness { Unknown, Signed, Unsigned };

static llvm::Module *getOwningModule(const llvm::Value *value) {
  if (!value)
    return nullptr;
  if (const auto *inst = llvm::dyn_cast<llvm::Instruction>(value))
    return const_cast<llvm::Module *>(inst->getModule());
  if (const auto *arg = llvm::dyn_cast<llvm::Argument>(value))
    return arg->getParent()
               ? const_cast<llvm::Module *>(arg->getParent()->getParent())
               : nullptr;
  if (const auto *gv = llvm::dyn_cast<llvm::GlobalValue>(value))
    return const_cast<llvm::Module *>(gv->getParent());
  return nullptr;
}

static uint32_t getTypeByteSize(const llvm::Type *type,
                                const llvm::DataLayout &dl) {
  if (!type)
    return 1;

  if (const auto *arrayTy = llvm::dyn_cast<llvm::ArrayType>(type))
    type = arrayTy->getElementType();
  else if (const auto *vecTy = llvm::dyn_cast<llvm::VectorType>(type))
    type = vecTy->getElementType();

  if (!type || !type->isSized())
    return 1;

  uint64_t size = dl.getTypeAllocSize(const_cast<llvm::Type *>(type));
  if (size == 0)
    return 1;
  if (size > MaxFieldLimit)
    return MaxFieldLimit;
  return static_cast<uint32_t>(size);
}

static uint32_t getElementByteSize(AbstractState &as, const llvm::Value *value) {
  llvm::Module *module = getOwningModule(value);
  if (!module)
    return 1;

  const llvm::DataLayout &dl = module->getDataLayout();
  if (value && value->getType()->isArrayTy())
    return getTypeByteSize(value->getType(), dl);

  if (!value || !value->getType()->isPointerTy())
    return 1;

  uint32_t valueId = AbstractInterpretation::getValueIdStatic(value);
  if (const llvm::Type *pointee = as.getPointeeElement(valueId))
    return getTypeByteSize(pointee, dl);

  return getTypeByteSize(value->getType()->getPointerElementType(), dl);
}

static uint32_t getTrackedByteCount(const IntervalValue &len,
                                    bool useLowerBound) {
  if (len.isBottom())
    return 0;
  if (len.is_infinite())
    return MaxFieldLimit;

  int64_t raw =
      useLowerBound ? len.lb().getIntNumeralOrZero() : len.ub().getIntNumeralOrZero();
  if (raw <= 0)
    return 0;
  if (raw > static_cast<int64_t>(MaxFieldLimit))
    return MaxFieldLimit;
  return static_cast<uint32_t>(raw);
}

static std::pair<uint32_t, int64_t>
resolvePointerBaseAndOffset(AbstractState &as, const llvm::Value *value,
                            uint32_t fallbackId) {
  const llvm::Value *base = value;
  int64_t byteOffset = 0;

  while (base) {
    if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(base)) {
      int64_t step = clampFieldOffset(as.getByteOffset(gep).ub());
      byteOffset = clampFieldOffset(byteOffset + step);
      base = gep->getPointerOperand();
      continue;
    }
    if (const auto *op = llvm::dyn_cast<llvm::Operator>(base)) {
      if (op->getOpcode() == llvm::Instruction::BitCast ||
          op->getOpcode() == llvm::Instruction::AddrSpaceCast) {
        base = op->getOperand(0);
        continue;
      }
    }
    break;
  }

  uint32_t baseId =
      base ? AbstractInterpretation::getValueIdStatic(base) : fallbackId;
  return {baseId, byteOffset};
}

static IntervalValue getIntegerRangeForBitWidth(unsigned bits,
                                                IntegerSignedness signHint) {
  if (bits <= 1)
    return IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(1));

  unsigned cappedBits = std::min(bits, 64u);
  auto signedRange = [cappedBits]() {
    if (cappedBits >= 64) {
      return IntervalValue(std::numeric_limits<int64_t>::min(),
                           std::numeric_limits<int64_t>::max());
    }
    int64_t lb = -(int64_t(1) << (cappedBits - 1));
    int64_t ub = (int64_t(1) << (cappedBits - 1)) - 1;
    return IntervalValue(lb, ub);
  };
  auto unsignedRange = [cappedBits]() {
    if (cappedBits >= 64)
      return IntervalValue(int64_t(0), std::numeric_limits<int64_t>::max());
    uint64_t ub = (uint64_t(1) << cappedBits) - 1;
    return IntervalValue(int64_t(0), static_cast<int64_t>(ub));
  };

  switch (signHint) {
  case IntegerSignedness::Signed:
    return signedRange();
  case IntegerSignedness::Unsigned:
    return unsignedRange();
  case IntegerSignedness::Unknown:
  default: {
    IntervalValue range = signedRange();
    range.join_with(unsignedRange());
    return range;
  }
  }
}

static IntervalValue getTypeRangeWithHint(llvm::Type *type,
                                          IntegerSignedness signHint) {
  if (!type)
    return IntervalValue::top();
  if (type->isIntegerTy())
    return getIntegerRangeForBitWidth(type->getIntegerBitWidth(), signHint);
  if (type->isFloatingPointTy()) {
    return IntervalValue(std::numeric_limits<int32_t>::min(),
                         std::numeric_limits<int32_t>::max());
  }
  return IntervalValue::top();
}

static void storeStringTerminator(AbstractState &as, uint32_t dstId,
                                  uint32_t byteOffset) {
  const llvm::Value *dstValue =
      AbstractInterpretation::getAEInstance().getValueFromIdStatic(dstId);
  const std::pair<uint32_t, int64_t> resolved =
      resolvePointerBaseAndOffset(as, dstValue, dstId);
  const uint32_t baseDstId = resolved.first;
  const int64_t baseOffset = resolved.second;
  AddressValue nullAddrs =
      as.getGepObjAddrs(baseDstId, IntervalValue(baseOffset + byteOffset));
  for (uint32_t addr : nullAddrs)
    as.store(addr, AbstractValue(IntervalValue(0)));
}

static std::vector<IntegerSignedness>
parseScanfSignednessHints(const std::string &format) {
  std::vector<IntegerSignedness> hints;
  for (size_t i = 0; i < format.size(); ++i) {
    if (format[i] != '%')
      continue;

    ++i;
    if (i >= format.size())
      break;
    if (format[i] == '%')
      continue;

    bool suppress = false;
    if (format[i] == '*') {
      suppress = true;
      ++i;
    }

    while (i < format.size() &&
           std::isdigit(static_cast<unsigned char>(format[i]))) {
      ++i;
    }

    if (i + 1 < format.size() &&
        ((format[i] == 'h' && format[i + 1] == 'h') ||
         (format[i] == 'l' && format[i + 1] == 'l'))) {
      i += 2;
    } else if (i < format.size() &&
               (format[i] == 'h' || format[i] == 'l' || format[i] == 'j' ||
                format[i] == 'z' || format[i] == 't' || format[i] == 'L')) {
      ++i;
    }

    if (i >= format.size())
      break;

    IntegerSignedness hint = IntegerSignedness::Unknown;
    switch (format[i]) {
    case 'd':
    case 'i':
    case 'n':
      hint = IntegerSignedness::Signed;
      break;
    case 'u':
    case 'o':
    case 'x':
    case 'X':
      hint = IntegerSignedness::Unsigned;
      break;
    default:
      hint = IntegerSignedness::Unknown;
      break;
    }

    if (!suppress)
      hints.push_back(hint);
  }
  return hints;
}

static IntegerSignedness getStrtoSignedness(llvm::StringRef funcName) {
  if (funcName.startswith("strtou"))
    return IntegerSignedness::Unsigned;
  if (funcName.startswith("strtol"))
    return IntegerSignedness::Signed;
  return IntegerSignedness::Unknown;
}

static const llvm::Value *stripConstantAddressBase(const llvm::Value *value) {
  while (value) {
    if (const auto *alias = llvm::dyn_cast<llvm::GlobalAlias>(value)) {
      value = alias->getAliasee();
      continue;
    }
    if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value)) {
      value = gep->getPointerOperand();
      continue;
    }
    if (const auto *op = llvm::dyn_cast<llvm::Operator>(value)) {
      if (op->getOpcode() == llvm::Instruction::BitCast ||
          op->getOpcode() == llvm::Instruction::AddrSpaceCast) {
        value = op->getOperand(0);
        continue;
      }
    }
    if (const auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(value)) {
      if (ce->getNumOperands() == 0)
        break;
      value = ce->getOperand(0);
      continue;
    }
    break;
  }
  return value;
}

static bool tryExtractConstantString(const llvm::Value *value,
                                     std::string &out) {
  value = stripConstantAddressBase(value);
  const auto *gv = llvm::dyn_cast_or_null<llvm::GlobalVariable>(value);
  if (!gv || !gv->hasInitializer())
    return false;

  const auto *data =
      llvm::dyn_cast<llvm::ConstantDataSequential>(gv->getInitializer());
  if (!data || !data->isCString())
    return false;

  out = data->getAsCString().str();
  return true;
}

bool isLikelyStdContainerSymbol(const std::string &funcName) {
  const bool hasContainerToken =
      containsAny(funcName, {"vector", "basic_string", "deque", "list", "map",
                             "unordered_map", "set", "unordered_set"});
  if (!hasContainerToken)
    return false;

  // Handle both demangled and Itanium-mangled variants:
  // - std::vector<T>::size()
  // - _ZNSt6vector... / _ZNSt3__16vector...
  // - basic_string with __cxx11 / libc++ namespaces
  return containsAny(funcName, {"std::", "St", "__cxx11", "3__1", "3__2"});
}

enum class STLContainerOpKind { None, SizeLike, EmptyLike, PointerLike };

STLContainerOpKind classifyStdContainerOp(const std::string &funcName) {
  if (containsAny(funcName, {"::size(", "4sizeEv", "sizeEv", "::length(",
                             "6lengthEv", "::capacity(", "8capacityEv"})) {
    return STLContainerOpKind::SizeLike;
  }

  if (containsAny(funcName, {"::empty(", "5emptyEv", "emptyEv"})) {
    return STLContainerOpKind::EmptyLike;
  }

  if (containsAny(funcName,
                  {"::data(", "4dataEv", "::c_str(", "5c_strEv", "::begin(",
                   "5beginEv", "::end(", "3endEv", "::front(", "5frontEv",
                   "::back(", "4backEv", "::at(", "2atE", "::find(", "4find",
                   "ixE", "::operator[]("})) {
    return STLContainerOpKind::PointerLike;
  }

  return STLContainerOpKind::None;
}

bool modelStdContainerCall(AEExtAPI &extApi, const llvm::CallBase *call,
                           const std::string &funcName) {
  if (!isLikelyStdContainerSymbol(funcName))
    return false;

  STLContainerOpKind opKind = classifyStdContainerOp(funcName);
  if (opKind == STLContainerOpKind::None)
    return false;

  if (call->getType()->isVoidTy())
    return true;

  AbstractState &as = extApi.getAbsStateFromTrace(call);
  uint32_t lhsId = AEExtAPI::getValueId(call);

  if (opKind == STLContainerOpKind::SizeLike) {
    as[lhsId] = AbstractValue(IntervalValue(0, MaxFieldLimit));
    return true;
  }

  if (opKind == STLContainerOpKind::EmptyLike) {
    as[lhsId] = AbstractValue(IntervalValue(0, 1));
    return true;
  }

  // Pointer/iterator/reference-like returns: conservatively alias `this`.
  if (call->arg_size() >= 1) {
    uint32_t thisId = AEExtAPI::getValueId(call->getArgOperand(0));
    if (as.inVarToAddrsTable(thisId)) {
      as[lhsId] = as[thisId];
      return true;
    }
  }

  as[lhsId] = AbstractValue(IntervalValue::top());
  return true;
}

} // namespace

AEExtAPI::AEExtAPI(std::map<const llvm::Instruction *, AbstractState> &traces)
    : module_(nullptr), abstractTrace(traces) {
  initAnnotationMap();
  initExtFunMap();
}

void AEExtAPI::initAnnotationMap() {
  addAnnotation("llvm.memcpy.p0i8.p0i8.i64",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});
  addAnnotation("llvm.memcpy.p0.p0.i64",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});
  addAnnotation("llvm.memcpy.p0i8.p0i8.i32",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});
  addAnnotation("llvm.memcpy.p0i8.p0i8.i16",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});
  addAnnotation("llvm.memcpy.p0i8.p0i8.i8",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});
  addAnnotation("llvm.memcpy",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});
  addAnnotation("llvm.memmove",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});
  addAnnotation("llvm.memmove.p0i8.p0i8.i64",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});
  addAnnotation("llvm.memmove.p0.p0.i64",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});
  addAnnotation("llvm.memmove.p0i8.p0i8.i32",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});
  addAnnotation("__memcpy_chk",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});
  addAnnotation("memmove",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});
  addAnnotation("bcopy",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});
  addAnnotation("memccpy",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg3", "BUF_CHECK:Arg1,Arg3"});
  addAnnotation("__memmove_chk",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});
  addAnnotation("__bcopy",
                {"MEMCPY", "BUF_CHECK:Arg0,Arg2", "BUF_CHECK:Arg1,Arg2"});

  addAnnotation("llvm.memset.p0i8.i32", {"MEMSET", "BUF_CHECK:Arg0,Arg2"});
  addAnnotation("llvm.memset.p0i8.i64", {"MEMSET", "BUF_CHECK:Arg0,Arg2"});
  addAnnotation("llvm.memset.p0.i64", {"MEMSET", "BUF_CHECK:Arg0,Arg2"});
  addAnnotation("llvm.memset.p0i8.i8", {"MEMSET", "BUF_CHECK:Arg0,Arg2"});
  addAnnotation("llvm.memset", {"MEMSET", "BUF_CHECK:Arg0,Arg2"});
  addAnnotation("__memset_chk", {"MEMSET", "BUF_CHECK:Arg0,Arg2"});
  addAnnotation("wmemset", {"MEMSET", "BUF_CHECK:Arg0,Arg2"});
  addAnnotation("bzero", {"MEMSET", "BUF_CHECK:Arg0,Arg1"});

  addAnnotation("strcpy", {"STRCPY", "BUF_CHECK:Arg0,Arg1"});
  addAnnotation("strncpy",
                {"STRCPY", "BUF_CHECK:Arg0,Arg1", "BUF_CHECK:Arg0,Arg2"});
  addAnnotation("stpcpy", {"STRCPY", "BUF_CHECK:Arg0,Arg1"});
  addAnnotation("strcat", {"STRCAT", "BUF_CHECK:Arg0,Arg1"});
  addAnnotation("strncat", {"STRCAT", "BUF_CHECK:Arg0,Arg1"});
  addAnnotation("__strcpy_chk", {"STRCPY", "BUF_CHECK:Arg0,Arg1"});
  addAnnotation("__strncpy_chk",
                {"STRCPY", "BUF_CHECK:Arg0,Arg1", "BUF_CHECK:Arg0,Arg2"});
  addAnnotation("__strcat_chk", {"STRCAT", "BUF_CHECK:Arg0,Arg1"});
  addAnnotation("__strncat_chk", {"STRCAT", "BUF_CHECK:Arg0,Arg1"});

  addAnnotation("wcscpy", {"STRCPY", "BUF_CHECK:Arg0,Arg1"});
  addAnnotation("wcsncpy",
                {"STRCPY", "BUF_CHECK:Arg0,Arg1", "BUF_CHECK:Arg0,Arg2"});
  addAnnotation("wcscat", {"STRCAT", "BUF_CHECK:Arg0,Arg1"});
  addAnnotation("wcsncat", {"STRCAT", "BUF_CHECK:Arg0,Arg1"});
  addAnnotation("__wcscpy_chk", {"STRCPY", "BUF_CHECK:Arg0,Arg1"});
  addAnnotation("__wcsncpy_chk",
                {"STRCPY", "BUF_CHECK:Arg0,Arg1", "BUF_CHECK:Arg0,Arg2"});
  addAnnotation("__wcscat_chk", {"STRCAT", "BUF_CHECK:Arg0,Arg1"});
  addAnnotation("__wcsncat_chk", {"STRCAT", "BUF_CHECK:Arg0,Arg1"});

  addAnnotation("malloc", {"ALLOC"});
  addAnnotation("calloc", {"ALLOC"});
  addAnnotation("realloc", {"REALLOC"});
  addAnnotation("free", {"FREE"});
  addAnnotation("cfree", {"FREE"});

  addAnnotation("strlen", {"STRLEN"});
  addAnnotation("wcslen", {"STRLEN"});

  addAnnotation("scanf", {"SCANF"});
  addAnnotation("__isoc99_scanf", {"SCANF"});
  addAnnotation("fscanf", {"SCANF"});
  addAnnotation("__isoc99_fscanf", {"SCANF"});
  addAnnotation("sscanf", {"SCANF"});
  addAnnotation("__isoc99_sscanf", {"SCANF"});
  addAnnotation("vscanf", {"SCANF"});
  addAnnotation("__isoc99_vscanf", {"SCANF"});

  addAnnotation("printf", {"PRINTF"});
  addAnnotation("fprintf", {"PRINTF"});
  addAnnotation("sprintf", {"PRINTF"});
  addAnnotation("snprintf", {"SNPRINTF"});
  addAnnotation("__snprintf_chk", {"SNPRINTF"});
  addAnnotation("vsnprintf", {"SNPRINTF"});
  addAnnotation("__vsnprintf_chk", {"SNPRINTF"});

  addAnnotation("recv", {"RECV"});
  addAnnotation("__recv", {"RECV"});

  addAnnotation("itoa", {"ITOA"});
  addAnnotation("_itoa", {"ITOA"});
  addAnnotation("_itoa_s", {"ITOA"});

  // Additional allocation functions from SVF's extapi.c
  addAnnotation("fopen", {"ALLOC"});
  addAnnotation("fopen64", {"ALLOC"});
  addAnnotation("fdopen", {"ALLOC"});
  addAnnotation("tmpfile64", {"ALLOC"});
  addAnnotation("tmpfile", {"ALLOC"});
  addAnnotation("tmpnam", {"ALLOC"});
  addAnnotation("tempnam", {"ALLOC"});
  addAnnotation("opendir", {"ALLOC"});
  addAnnotation("readdir", {"ALLOC"});
  addAnnotation("readdir64", {"ALLOC"});
  addAnnotation("strdup", {"ALLOC"});
  addAnnotation("strndup", {"ALLOC"});
  addAnnotation("__strdup", {"ALLOC"});
  addAnnotation("getlogin", {"ALLOC"});
  addAnnotation("getpass", {"ALLOC"});
  addAnnotation("getenv", {"ALLOC"});
  addAnnotation("dlopen", {"ALLOC"});
  addAnnotation("dlerror", {"ALLOC"});
  addAnnotation("crypt", {"ALLOC"});
  addAnnotation("inet_ntoa", {"ALLOC"});
  addAnnotation("getcwd", {"REALLOC"});
  addAnnotation("gethostname", {"ALLOC"});
  addAnnotation("getsockname", {"ALLOC"});
  addAnnotation("getpeername", {"ALLOC"});
  addAnnotation("malloc", {"ALLOC"});
  addAnnotation("calloc", {"ALLOC"});
  addAnnotation("realloc", {"REALLOC"});
  addAnnotation("reallocarray", {"REALLOC"});
  addAnnotation("memalign", {"ALLOC"});
  addAnnotation("aligned_alloc", {"ALLOC"});
  addAnnotation("posix_memalign", {"ALLOC_ARG0"});
  addAnnotation("valloc", {"ALLOC"});
  addAnnotation("pvalloc", {"ALLOC"});
  addAnnotation("mmap", {"ALLOC"});
  addAnnotation("mmap64", {"ALLOC"});
  addAnnotation("mremap", {"REALLOC"});
  addAnnotation("sbrk", {"ALLOC"});
  addAnnotation("asprintf", {"ALLOC_ARG0"});
  addAnnotation("vasprintf", {"ALLOC_ARG0"});

  // C++ operators
  addAnnotation("_Znwj", {"ALLOC"}); // operator new(unsigned int)
  addAnnotation("_ZnwjRKSt9nothrow_t", {"ALLOC"});
  addAnnotation("_ZnwjSt11align_val_t", {"ALLOC"});
  addAnnotation("_ZnwjSt11align_val_tRKSt9nothrow_t", {"ALLOC"});
  addAnnotation("_Znaj", {"ALLOC"}); // operator new[](unsigned int)
  addAnnotation("_ZnajRKSt9nothrow_t", {"ALLOC"});
  addAnnotation("_ZnajSt11align_val_t", {"ALLOC"});
  addAnnotation("_ZnajSt11align_val_tRKSt9nothrow_t", {"ALLOC"});
  addAnnotation("_Znwm", {"ALLOC"}); // operator new(unsigned long)
  addAnnotation("_ZnwmRKSt9nothrow_t", {"ALLOC"});
  addAnnotation("_ZnwmSt11align_val_t", {"ALLOC"});
  addAnnotation("_ZnwmSt11align_val_tRKSt9nothrow_t", {"ALLOC"});
  addAnnotation("_Znam", {"ALLOC"}); // operator new[](unsigned long)
  addAnnotation("_ZnamRKSt9nothrow_t", {"ALLOC"});
  addAnnotation("_ZnamSt11align_val_t", {"ALLOC"});
  addAnnotation("_ZnamSt11align_val_tRKSt9nothrow_t", {"ALLOC"});

  // zalloc variants
  addAnnotation("zmalloc", {"ALLOC"});
  addAnnotation("xcalloc", {"ALLOC"});
  addAnnotation("xmalloc", {"ALLOC"});
  addAnnotation("xrealloc", {"REALLOC"});
  addAnnotation("safe_malloc", {"ALLOC"});
  addAnnotation("safe_calloc", {"ALLOC"});
  addAnnotation("safe_realloc", {"REALLOC"});

  // Additional string functions
  addAnnotation("strtok", {"STRTOK"});
  addAnnotation("strtok_r", {"STRTOK"});
  addAnnotation("strsep", {"STRTOK"});
  addAnnotation("strchr", {"STRCHR"});
  addAnnotation("strrchr", {"STRCHR"});
  addAnnotation("strstr", {"STRSTR"});
  addAnnotation("strcasestr", {"STRSTR"});
  addAnnotation("strpbrk", {"STRPBRK"});
  addAnnotation("index", {"STRCHR"});
  addAnnotation("rindex", {"STRCHR"});

  // Memory functions
  addAnnotation("bzero", {"MEMSET"});
  addAnnotation("index", {"MEMCPY"});
  addAnnotation("rindex", {"MEMCPY"});

  // Additional I/O
  addAnnotation("fgets", {"FGETS"});
  addAnnotation("fread", {"FREAD"});
  addAnnotation("fwrite", {"FWRITE"});
  addAnnotation("fopen", {"ALLOC"});
  addAnnotation("freopen", {"ALLOC"});
  addAnnotation("freopen64", {"ALLOC"});
  addAnnotation("popen", {"ALLOC"});
  addAnnotation("setmntent", {"ALLOC"});
  addAnnotation("shmat", {"ALLOC"});

  // Time functions
  addAnnotation("gmtime", {"TIME"});
  addAnnotation("localtime", {"TIME"});
  addAnnotation("ctime", {"TIME"});
  addAnnotation("asctime", {"TIME"});
  addAnnotation("getenv", {"ENV"});

  // Math functions
  addAnnotation("strtod", {"STRTO"});
  addAnnotation("strtof", {"STRTO"});
  addAnnotation("strtold", {"STRTO"});
  addAnnotation("strtol", {"STRTO"});
  addAnnotation("strtoll", {"STRTO"});
  addAnnotation("strtoul", {"STRTO"});
  addAnnotation("strtoull", {"STRTO"});
}

void AEExtAPI::addAnnotation(const std::string &funcName,
                             const std::vector<std::string> &annotations) {
  funcAnnotations[funcName] = annotations;
}

std::vector<std::string>
AEExtAPI::getExtFuncAnnotations(const llvm::Function *fun) {
  if (!fun)
    return {};

  std::string funcName = fun->getName().str();

  auto it = funcAnnotations.find(funcName);
  if (it != funcAnnotations.end()) {
    return it->second;
  }

  return {};
}

bool AEExtAPI::hasExtFuncAnnotation(const llvm::Function *fun,
                                    const std::string &annotation) {
  std::vector<std::string> annotations = getExtFuncAnnotations(fun);
  for (const auto &ann : annotations) {
    if (ann.find(annotation) != std::string::npos) {
      return true;
    }
  }
  return false;
}

AEExtAPI::ExtAPIType AEExtAPI::getExtAPIType(const llvm::Function *fun) {
  if (!fun)
    return UNCLASSIFIED;

  for (const std::string &annotation : getExtFuncAnnotations(fun)) {
    if (annotation.find("MEMCPY") != std::string::npos)
      return MEMCPY;
    if (annotation.find("MEMSET") != std::string::npos)
      return MEMSET;
    if (annotation.find("STRCPY") != std::string::npos)
      return STRCPY;
    if (annotation.find("STRCAT") != std::string::npos)
      return STRCAT;
    if (annotation.find("ALLOC_ARG0") != std::string::npos)
      return ALLOC_ARG0;
    if (annotation.find("REALLOC") != std::string::npos)
      return REALLOC;
    if (annotation.find("ALLOC") != std::string::npos)
      return ALLOC;
    if (annotation.find("FREE") != std::string::npos)
      return FREE;
    if (annotation.find("STRLEN") != std::string::npos)
      return STRLEN;
    if (annotation.find("SCANF") != std::string::npos)
      return SCANF;
    if (annotation.find("PRINTF") != std::string::npos)
      return PRINTF;
    if (annotation.find("RECV") != std::string::npos)
      return RECV;
    if (annotation.find("ITOA") != std::string::npos)
      return ITOA;
    if (annotation.find("SNPRINTF") != std::string::npos)
      return SNPRINTF;
    // Additional types
    if (annotation.find("STRTOK") != std::string::npos)
      return STRTOK;
    if (annotation.find("STRCHR") != std::string::npos)
      return STRCHR;
    if (annotation.find("STRSTR") != std::string::npos)
      return STRSTR;
    if (annotation.find("FGETS") != std::string::npos)
      return FGETS;
    if (annotation.find("FREAD") != std::string::npos)
      return FREAD;
    if (annotation.find("FWRITE") != std::string::npos)
      return FWRITE;
    if (annotation.find("TIME") != std::string::npos)
      return TIME;
    if (annotation.find("ENV") != std::string::npos)
      return ENV;
    if (annotation.find("STRTO") != std::string::npos)
      return STRTO;
  }

  return UNCLASSIFIED;
}

void AEExtAPI::initExtFunMap() {
  // Ctype functions - return [0, 1] for character classification
  auto sse_isalnum = [this](const llvm::CallBase *callNode) {
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(IntervalValue(0, 1));
  };
  func_map["isalnum"] = sse_isalnum;
  func_map["isalpha"] = sse_isalnum;
  func_map["isdigit"] = sse_isalnum;
  func_map["isprint"] = sse_isalnum;
  func_map["isupper"] = sse_isalnum;
  func_map["islower"] = sse_isalnum;
  func_map["isblank"] = sse_isalnum;
  func_map["isspace"] = sse_isalnum;
  func_map["iscntrl"] = sse_isalnum;
  func_map["isgraph"] = sse_isalnum;
  func_map["ispunct"] = sse_isalnum;
  func_map["isxdigit"] = sse_isalnum;

  // Math functions - match SVF's SSE_FUNC_PROCESS macro behavior
  // Each math function is explicitly defined (matching SVF's approach)

  auto sse_sin = [this](const llvm::CallBase *callNode) {
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t argId = getValueId(callNode->getArgOperand(0));
    if (!as.inVarToValTable(argId)) {
      uint32_t lhsId = getValueId(callNode);
      as[lhsId] = AbstractValue(IntervalValue(-1, 1));
      return;
    }
    double rhs =
        static_cast<double>(as[argId].getInterval().lb().getIntNumeralOrZero());
    double res = sin(rhs);
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(IntervalValue(res, res));
  };
  func_map["sin"] = sse_sin;
  func_map["llvm.sin.f64"] = sse_sin;

  auto sse_cos = [this](const llvm::CallBase *callNode) {
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t argId = getValueId(callNode->getArgOperand(0));
    if (!as.inVarToValTable(argId)) {
      uint32_t lhsId = getValueId(callNode);
      as[lhsId] = AbstractValue(IntervalValue(-1, 1));
      return;
    }
    double rhs =
        static_cast<double>(as[argId].getInterval().lb().getIntNumeralOrZero());
    double res = cos(rhs);
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(IntervalValue(res, res));
  };
  func_map["cos"] = sse_cos;
  func_map["llvm.cos.f64"] = sse_cos;

  auto sse_tan = [this](const llvm::CallBase *callNode) {
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t argId = getValueId(callNode->getArgOperand(0));
    if (!as.inVarToValTable(argId)) {
      uint32_t lhsId = getValueId(callNode);
      as[lhsId] = AbstractValue(IntervalValue::top());
      return;
    }
    double rhs =
        static_cast<double>(as[argId].getInterval().lb().getIntNumeralOrZero());
    double res = tan(rhs);
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(IntervalValue(res, res));
  };
  func_map["tan"] = sse_tan;
  func_map["llvm.tan.f64"] = sse_tan;

  auto sse_log = [this](const llvm::CallBase *callNode) {
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t argId = getValueId(callNode->getArgOperand(0));
    if (!as.inVarToValTable(argId)) {
      uint32_t lhsId = getValueId(callNode);
      as[lhsId] = AbstractValue(IntervalValue::top());
      return;
    }
    double rhs =
        static_cast<double>(as[argId].getInterval().lb().getIntNumeralOrZero());
    double res = log(rhs);
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(IntervalValue(res, res));
  };
  func_map["log"] = sse_log;
  func_map["llvm.log.f64"] = sse_log;

  auto sse_sinh = [this](const llvm::CallBase *callNode) {
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t argId = getValueId(callNode->getArgOperand(0));
    if (!as.inVarToValTable(argId)) {
      uint32_t lhsId = getValueId(callNode);
      as[lhsId] = AbstractValue(IntervalValue::top());
      return;
    }
    double rhs =
        static_cast<double>(as[argId].getInterval().lb().getIntNumeralOrZero());
    double res = sinh(rhs);
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(IntervalValue(res, res));
  };
  func_map["sinh"] = sse_sinh;

  auto sse_cosh = [this](const llvm::CallBase *callNode) {
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t argId = getValueId(callNode->getArgOperand(0));
    if (!as.inVarToValTable(argId)) {
      uint32_t lhsId = getValueId(callNode);
      as[lhsId] = AbstractValue(IntervalValue::top());
      return;
    }
    double rhs =
        static_cast<double>(as[argId].getInterval().lb().getIntNumeralOrZero());
    double res = cosh(rhs);
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(IntervalValue(res, res));
  };
  func_map["cosh"] = sse_cosh;

  auto sse_tanh = [this](const llvm::CallBase *callNode) {
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t argId = getValueId(callNode->getArgOperand(0));
    if (!as.inVarToValTable(argId)) {
      uint32_t lhsId = getValueId(callNode);
      as[lhsId] = AbstractValue(IntervalValue::top());
      return;
    }
    double rhs =
        static_cast<double>(as[argId].getInterval().lb().getIntNumeralOrZero());
    double res = tanh(rhs);
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(IntervalValue(res, res));
  };
  func_map["tanh"] = sse_tanh;

  auto sse_sqrt = [this](const llvm::CallBase *callNode) {
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t argId = getValueId(callNode->getArgOperand(0));
    if (!as.inVarToValTable(argId)) {
      uint32_t lhsId = getValueId(callNode);
      as[lhsId] = AbstractValue(IntervalValue(0, 1000000));
      return;
    }
    double rhs =
        static_cast<double>(as[argId].getInterval().lb().getIntNumeralOrZero());
    double res = sqrt(rhs);
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(IntervalValue(res, res));
  };
  func_map["sqrt"] = sse_sqrt;

  // Assertion functions
  auto sse_svf_assert = [this](const llvm::CallBase *callNode) {
    AbstractInterpretation::getAEInstance().markCheckpointChecked(callNode);
    AbstractInterpretation::getAEInstance().checkpoints.erase(callNode);
    if (callNode->arg_size() < 1)
      return;
    uint32_t arg0 = getValueId(callNode->getArgOperand(0));
    AbstractState &as = getAbsStateFromTrace(callNode);
    if (as.inVarToValTable(arg0) &&
        as[arg0].getInterval().equals(IntervalValue(1, 1))) {
      llvm::outs() << "Assertion verified successfully\n";
    } else {
      llvm::errs() << "Assertion failure!\n";
    }
  };
  func_map["svf_assert"] = sse_svf_assert;

  // String functions
  auto sse_strlen = [this](const llvm::CallBase *callNode) {
    if (callNode->arg_size() < 1)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t strId = getValueId(callNode->getArgOperand(0));
    IntervalValue len = getStrlen(as, strId);
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(len);
  };
  func_map["strlen"] = sse_strlen;
  func_map["wcslen"] = sse_strlen;

  auto sse_strcpy = [this](const llvm::CallBase *callNode) {
    if (callNode->arg_size() < 2)
      return;
    handleStrcpy(callNode);
    uint32_t lhsId = getValueId(callNode);
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t dstId = getValueId(callNode->getArgOperand(0));
    as[lhsId] = as[dstId];
  };
  func_map["strcpy"] = sse_strcpy;
  func_map["strncpy"] = sse_strcpy;
  func_map["wcscpy"] = sse_strcpy;
  func_map["wcsncpy"] = sse_strcpy;
  func_map["__strcpy_chk"] = sse_strcpy;
  func_map["__strncpy_chk"] = sse_strcpy;
  func_map["__wcscpy_chk"] = sse_strcpy;
  func_map["__wcsncpy_chk"] = sse_strcpy;

  // stpcpy - returns pointer to end (null terminator) of dst
  auto sse_stpcpy = [this](const llvm::CallBase *callNode) {
    if (callNode->arg_size() < 2)
      return;
    handleStrcpy(callNode);
    uint32_t lhsId = getValueId(callNode);
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t dstId = getValueId(callNode->getArgOperand(0));
    // stpcpy returns pointer to the terminating null character (simplified:
    // return dst)
    as[lhsId] = as[dstId];
  };
  func_map["stpcpy"] = sse_stpcpy;

  auto sse_strcat = [this](const llvm::CallBase *callNode) {
    if (callNode->arg_size() < 2)
      return;
    handleStrcat(callNode);
    uint32_t lhsId = getValueId(callNode);
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t dstId = getValueId(callNode->getArgOperand(0));
    as[lhsId] = as[dstId];
  };
  func_map["strcat"] = sse_strcat;
  func_map["strncat"] = sse_strcat;
  func_map["wcscat"] = sse_strcat;
  func_map["wcsncat"] = sse_strcat;
  func_map["__strcat_chk"] = sse_strcat;
  func_map["__strncat_chk"] = sse_strcat;
  func_map["__wcscat_chk"] = sse_strcat;
  func_map["__wcsncat_chk"] = sse_strcat;

  // Memory allocation functions
  auto sse_malloc = [this](const llvm::CallBase *callNode) {
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t lhsId = getValueId(callNode);
    uint32_t newAddr = AddressValue::getVirtualMemAddress(lhsId);
    as[lhsId] = AbstractValue(AddressValue(newAddr));

    // Track object size for buffer overflow detection
    if (callNode->arg_size() >= 1) {
      uint32_t sizeId = getValueId(callNode->getArgOperand(0));
      uint32_t objId = AddressValue::getInternalID(newAddr);
      as.addHeapObject(objId);

      if (const auto *csize =
              llvm::dyn_cast<llvm::ConstantInt>(callNode->getArgOperand(0))) {
        as.setObjSize(objId, static_cast<uint32_t>(csize->getZExtValue()));
      } else if (as.inVarToValTable(sizeId)) {
        IntervalValue size = as[sizeId].getInterval();
        if (size.is_numeral()) {
          // Exact size known
          as.setObjSize(objId,
                        static_cast<uint32_t>(size.getIntNumeralOrZero()));
        } else {
          // Size is an interval - use upper bound conservatively
          int64_t ub = size.ub().getIntNumeralOrZero();
          if (ub > 0 && ub <= static_cast<int64_t>(MaxFieldLimit)) {
            as.setObjSize(objId, static_cast<uint32_t>(ub));
          } else {
            // Conservative: use MaxFieldLimit if size is unknown or too large
            as.setObjSize(objId, MaxFieldLimit);
          }
        }
      } else {
        // Size unknown - use conservative default
        as.setObjSize(objId, MaxFieldLimit);
      }
    }
  };

  auto sse_calloc = [this](const llvm::CallBase *callNode) {
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t lhsId = getValueId(callNode);
    uint32_t newAddr = AddressValue::getVirtualMemAddress(lhsId);
    as[lhsId] = AbstractValue(AddressValue(newAddr));

    // calloc(nmemb, size) - total size is nmemb * size
    if (callNode->arg_size() >= 2) {
      uint32_t nmembId = getValueId(callNode->getArgOperand(0));
      uint32_t sizeId = getValueId(callNode->getArgOperand(1));
      uint32_t objId = AddressValue::getInternalID(newAddr);
      as.addHeapObject(objId);

      IntervalValue nmemb(1, MaxFieldLimit);
      IntervalValue size(1, MaxFieldLimit);

      if (as.inVarToValTable(nmembId)) {
        nmemb = as[nmembId].getInterval();
      }
      if (as.inVarToValTable(sizeId)) {
        size = as[sizeId].getInterval();
      }

      // Calculate total size: nmemb * size
      int64_t totalSize =
          nmemb.ub().getIntNumeralOrZero() * size.ub().getIntNumeralOrZero();
      if (totalSize > 0 && totalSize <= static_cast<int64_t>(MaxFieldLimit)) {
        as.setObjSize(objId, static_cast<uint32_t>(totalSize));
      } else {
        as.setObjSize(objId, MaxFieldLimit);
      }
    }
  };

  func_map["malloc"] = sse_malloc;
  func_map["calloc"] = sse_calloc;
  auto sse_realloc = [this](const llvm::CallBase *callNode) {
    handleExtRealloc(callNode);
  };
  func_map["realloc"] = sse_realloc;

  // Memory functions
  auto sse_memcpy = [this](const llvm::CallBase *callNode) {
    if (callNode->arg_size() < 3)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t dstId = getValueId(callNode->getArgOperand(0));
    uint32_t srcId = getValueId(callNode->getArgOperand(1));
    uint32_t lenId = getValueId(callNode->getArgOperand(2));

    IntervalValue len(0, 4096);
    if (as.inVarToValTable(lenId)) {
      len = as[lenId].getInterval();
    }
    handleMemcpy(as, dstId, srcId, len, 0);

    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = as[dstId];
  };
  func_map["memcpy"] = sse_memcpy;
  func_map["memmove"] = sse_memcpy;
  func_map["memccpy"] = sse_memcpy;
  func_map["llvm.memcpy.p0i8.p0i8.i64"] = sse_memcpy;
  func_map["llvm.memcpy.p0.p0.i64"] = sse_memcpy;
  func_map["llvm.memcpy.p0i8.p0i8.i32"] = sse_memcpy;
  func_map["llvm.memcpy.p0i8.p0i8.i16"] = sse_memcpy;
  func_map["llvm.memcpy.p0i8.p0i8.i8"] = sse_memcpy;
  func_map["llvm.memmove.p0i8.p0i8.i64"] = sse_memcpy;
  func_map["llvm.memmove.p0.p0.i64"] = sse_memcpy;
  func_map["llvm.memmove.p0i8.p0i8.i32"] = sse_memcpy;
  func_map["__memcpy_chk"] = sse_memcpy;
  func_map["__memmove_chk"] = sse_memcpy;
  func_map["bcopy"] = sse_memcpy;
  func_map["__bcopy"] = sse_memcpy;

  auto sse_memset = [this](const llvm::CallBase *callNode) {
    if (callNode->arg_size() < 3)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t dstId = getValueId(callNode->getArgOperand(0));
    uint32_t valId = getValueId(callNode->getArgOperand(1));
    uint32_t lenId = getValueId(callNode->getArgOperand(2));

    IntervalValue elem(0, 255);
    if (as.inVarToValTable(valId)) {
      elem = as[valId].getInterval();
    }
    IntervalValue len(0, 4096);
    if (as.inVarToValTable(lenId)) {
      len = as[lenId].getInterval();
    }
    handleMemset(as, dstId, elem, len);

    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = as[dstId];
  };
  func_map["memset"] = sse_memset;
  func_map["llvm.memset.p0i8.i64"] = sse_memset;
  func_map["llvm.memset.p0i8.i32"] = sse_memset;
  func_map["llvm.memset.p0.i64"] = sse_memset;
  func_map["llvm.memset.p0i8.i8"] = sse_memset;
  func_map["llvm.memset"] = sse_memset;
  func_map["__memset_chk"] = sse_memset;

  // bzero - zero out memory (like memset(dst, 0, len))
  auto sse_bzero = [this](const llvm::CallBase *callNode) {
    if (callNode->arg_size() < 2)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t dstId = getValueId(callNode->getArgOperand(0));
    uint32_t lenId = getValueId(callNode->getArgOperand(1));
    IntervalValue len(0, 4096);
    if (as.inVarToValTable(lenId)) {
      len = as[lenId].getInterval();
    }
    handleMemset(as, dstId, IntervalValue(0, 0), len);
  };
  func_map["bzero"] = sse_bzero;

  // wmemset - wide character memset
  func_map["wmemset"] = sse_memset;

  auto sse_free = [this](const llvm::CallBase *callNode) {
    handleExtFree(callNode);
  };
  func_map["free"] = sse_free;
  func_map["cfree"] = sse_free;
  func_map["xfree"] = sse_free;
  func_map["safe_free"] = sse_free;
  func_map["safefree"] = sse_free;
  // Add all free-related functions from SVF
  func_map["VOS_MemFree"] = sse_free;
  func_map["free_all_mem"] = sse_free;
  func_map["freeaddrinfo"] = sse_free;
  func_map["gcry_mpi_release"] = sse_free;
  func_map["gcry_sexp_release"] = sse_free;
  func_map["globfree"] = sse_free;
  func_map["nhfree"] = sse_free;
  func_map["obstack_free"] = sse_free;
  func_map["safe_cfree"] = sse_free;
  func_map["safexfree"] = sse_free;
  func_map["sm_free"] = sse_free;
  func_map["vim_free"] = sse_free;
  func_map["SSL_CTX_free"] = sse_free;
  func_map["SSL_free"] = sse_free;
  func_map["XFree"] = sse_free;

  // Input functions (scanf, fscanf, sscanf)
  auto sse_scanf = [this](const llvm::CallBase *callNode) {
    handleExtScanf(callNode);
  };

  auto sse_fscanf = [this](const llvm::CallBase *callNode) {
    handleExtScanf(callNode);
  };

  func_map["scanf"] = sse_scanf;
  func_map["__isoc99_scanf"] = sse_scanf;
  func_map["__isoc99_vscanf"] = sse_scanf;
  func_map["vscanf"] = sse_scanf;
  func_map["fscanf"] = sse_fscanf;
  func_map["__isoc99_fscanf"] = sse_fscanf;
  func_map["sscanf"] = sse_scanf;
  func_map["__isoc99_sscanf"] = sse_scanf;

  // Assertion functions
  auto sse_svf_assert_eq = [this](const llvm::CallBase *callNode) {
    AbstractInterpretation::getAEInstance().markCheckpointChecked(callNode);
    AbstractInterpretation::getAEInstance().checkpoints.erase(callNode);
    if (callNode->arg_size() < 2)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t arg0 = getValueId(callNode->getArgOperand(0));
    uint32_t arg1 = getValueId(callNode->getArgOperand(1));

    if (as.inVarToValTable(arg0) && as.inVarToValTable(arg1)) {
      if (as[arg0].getInterval().equals(as[arg1].getInterval())) {
        llvm::outs() << "Assertion verified successfully\n";
      } else {
        llvm::errs() << "Assertion failure: values not equal\n";
      }
    }
  };
  func_map["svf_assert_eq"] = sse_svf_assert_eq;

  // Print function (for debugging) - matching SVF's svf_print
  auto sse_svf_print = [this](const llvm::CallBase *callNode) {
    if (callNode->arg_size() < 2)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t numId = getValueId(callNode->getArgOperand(0));
    uint32_t strId = getValueId(callNode->getArgOperand(1));

    if (as.inVarToValTable(numId)) {
      IntervalValue itv = as[numId].getInterval();
      std::string str = strRead(as, strId);
      llvm::outs() << "Text: " << str
                   << ", Value: " << callNode->getArgOperand(0)->getName().str()
                   << ", PrintVal: " << itv.toString() << ", Loc:" << "\n";
    }
  };
  func_map["svf_print"] = sse_svf_print;

  // Network functions
  auto sse_recv = [this](const llvm::CallBase *callNode) {
    if (callNode->arg_size() < 4)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t lenId = getValueId(callNode->getArgOperand(2));

    constexpr uint32_t MaxRecvLen = 10000;
    IntervalValue len(0, MaxRecvLen);
    if (as.inVarToValTable(lenId)) {
      len = as[lenId].getInterval() - IntervalValue(1);
      if (len.lb().getIntNumeralOrZero() < 0) {
        len = IntervalValue(0, len.ub().getIntNumeralOrZero());
      }
    }

    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(len);
  };
  func_map["recv"] = sse_recv;
  func_map["__recv"] = sse_recv;

  // svf_set_value - Set value range for variables (matching SVF signature)
  auto sse_svf_set_value = [this](const llvm::CallBase *callNode) {
    if (callNode->arg_size() < 3)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t varId = getValueId(callNode->getArgOperand(0));
    uint32_t lbId = getValueId(callNode->getArgOperand(1));
    uint32_t ubId = getValueId(callNode->getArgOperand(2));

    if (!as.inVarToValTable(lbId) || !as.inVarToValTable(ubId))
      return;

    const IntervalValue &lb = as[lbId].getInterval();
    const IntervalValue &ub = as[ubId].getInterval();

    if (!lb.is_numeral() || !ub.is_numeral())
      return;

    if (!as.inVarToValTable(varId))
      as[varId] = AbstractValue(IntervalValue::top());

    as[varId].getInterval().meet_with(IntervalValue(lb.lb(), ub.ub()));
  };
  func_map["svf_set_value"] = sse_svf_set_value;
  func_map["set_value"] = sse_svf_set_value;

  // itoa - Integer to ASCII conversion
  auto sse_itoa = [this](const llvm::CallBase *callNode) {
    if (callNode->arg_size() < 2)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t strId = getValueId(callNode->getArgOperand(1));

    // Model itoa as writing a string representation of the number
    // Conservative: assume string length is [1, 20] (for 64-bit integers)
    IntervalValue strLen(1, 20);

    if (as.inVarToAddrsTable(strId)) {
      // Store string length information
      // The actual string content is modeled conservatively
      for (auto addr : as[strId].getAddrs()) {
        // Store null terminator at the end
        uint32_t objId = as.getIDFromAddr(addr);
        uint32_t objSize = as.getObjSize(objId);
        if (objSize > 0) {
          AddressValue nullAddr =
              as.getGepObjAddrs(strId, IntervalValue(objSize - 1));
          for (auto nullAddrVal : nullAddr) {
            as.store(nullAddrVal, AbstractValue(IntervalValue(0)));
          }
        }
      }
    }

    // Return value is the string pointer
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = as[strId];
  };
  func_map["itoa"] = sse_itoa;
  func_map["_itoa"] = sse_itoa;
  func_map["_itoa_s"] = sse_itoa;

  // snprintf - Bounded string formatting
  auto sse_snprintf = [this](const llvm::CallBase *callNode) {
    if (callNode->arg_size() < 3)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t strId = getValueId(callNode->getArgOperand(0));
    uint32_t sizeId = getValueId(callNode->getArgOperand(1));

    // Get the size limit
    IntervalValue maxSize(0, MaxFieldLimit);
    if (as.inVarToValTable(sizeId)) {
      maxSize = as[sizeId].getInterval();
      if (maxSize.ub().getIntNumeralOrZero() >
          static_cast<int64_t>(MaxFieldLimit)) {
        maxSize = IntervalValue(0, MaxFieldLimit);
      }
    }

    // Model snprintf as writing a string with length bounded by size
    if (as.inVarToAddrsTable(strId)) {
      // Store null terminator at the end (within size limit)
      uint32_t maxLen =
          static_cast<uint32_t>(maxSize.ub().getIntNumeralOrZero());
      if (maxLen > 0 && maxLen < MaxFieldLimit) {
        AddressValue nullAddr =
            as.getGepObjAddrs(strId, IntervalValue(maxLen - 1));
        for (auto nullAddrVal : nullAddr) {
          as.store(nullAddrVal, AbstractValue(IntervalValue(0)));
        }
      }
    }

    // Return value is the number of characters written (excluding null
    // terminator)
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(maxSize);
  };
  func_map["snprintf"] = sse_snprintf;
  func_map["_snprintf"] = sse_snprintf;
  func_map["_snprintf_s"] = sse_snprintf;
  func_map["__snprintf_chk"] = sse_snprintf;
  func_map["__vsnprintf_chk"] = sse_snprintf;
  func_map["vsnprintf"] = sse_snprintf;
  func_map["swprintf"] = sse_snprintf;
  func_map["_snwprintf"] = sse_snprintf;

  // sprintf - Unbounded string formatting (dangerous, but model it)
  auto sse_sprintf = [this](const llvm::CallBase *callNode) {
    if (callNode->arg_size() < 2)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t strId = getValueId(callNode->getArgOperand(0));

    // Model sprintf conservatively - assume it writes a string
    // Conservative: assume string length is [0, MaxFieldLimit]
    IntervalValue strLen(0, MaxFieldLimit);

    if (as.inVarToAddrsTable(strId)) {
      // Store null terminator at a conservative position
      uint32_t objId = as.getIDFromAddr(*as[strId].getAddrs().begin());
      uint32_t objSize = as.getObjSize(objId);
      if (objSize > 0) {
        uint32_t nullPos =
            (objSize < MaxFieldLimit) ? objSize - 1 : MaxFieldLimit - 1;
        AddressValue nullAddr =
            as.getGepObjAddrs(strId, IntervalValue(nullPos));
        for (auto nullAddrVal : nullAddr) {
          as.store(nullAddrVal, AbstractValue(IntervalValue(0)));
        }
      }
    }

    // Return value is the number of characters written
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(strLen);
  };
  func_map["sprintf"] = sse_sprintf;
  func_map["_sprintf"] = sse_sprintf;
  func_map["_sprintf_s"] = sse_sprintf;
  func_map["__sprintf_chk"] = sse_sprintf;
  // These match SVF's behavior (empty implementation)
  func_map["vsprintf"] = sse_sprintf;
  func_map["__vsprintf_chk"] = sse_sprintf;

  // fread - File reading
  auto sse_fread = [this](const llvm::CallBase *callNode) {
    if (callNode->arg_size() < 4)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t ptrId = getValueId(callNode->getArgOperand(0));
    uint32_t sizeId = getValueId(callNode->getArgOperand(1));
    uint32_t nmembId = getValueId(callNode->getArgOperand(2));

    // Calculate total bytes to read: size * nmemb
    IntervalValue size(1, MaxFieldLimit);
    IntervalValue nmemb(1, MaxFieldLimit);
    if (as.inVarToValTable(sizeId)) {
      size = as[sizeId].getInterval();
    }
    if (as.inVarToValTable(nmembId)) {
      nmemb = as[nmembId].getInterval();
    }

    // Total bytes = size * nmemb (conservative multiplication)
    int64_t totalBytes =
        size.ub().getIntNumeralOrZero() * nmemb.ub().getIntNumeralOrZero();
    if (totalBytes > static_cast<int64_t>(MaxFieldLimit)) {
      totalBytes = MaxFieldLimit;
    }

    // Model fread as reading data into the buffer
    if (as.inVarToAddrsTable(ptrId)) {
      // Store conservative values (TOP) to the buffer
      AbstractValue readVal(IntervalValue::top());
      for (uint32_t i = 0;
           i < static_cast<uint32_t>(totalBytes) && i < MaxFieldLimit; ++i) {
        AddressValue addrs = as.getGepObjAddrs(ptrId, IntervalValue(i));
        for (auto addr : addrs) {
          as.store(addr, readVal);
        }
      }
    }

    // Return value is the number of items read (could be less than nmemb)
    uint32_t lhsId = getValueId(callNode);
    IntervalValue itemsRead(0, nmemb.ub().getIntNumeralOrZero());
    as[lhsId] = AbstractValue(itemsRead);
  };
  func_map["fread"] = sse_fread;

  // Additional memory allocation functions from SVF
  auto sse_zmalloc = [this](const llvm::CallBase *callNode) {
    // zmalloc(size) - allocates and zeros memory
    // Just use malloc semantics for now
    if (callNode->arg_size() < 1)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t lhsId = getValueId(callNode);
    uint32_t newAddr = AddressValue::getVirtualMemAddress(lhsId);
    AddressValue addr(newAddr);
    AbstractValue addrVal = addr;
    as[lhsId] = addrVal;
    if (callNode->arg_size() >= 1) {
      uint32_t sizeId = getValueId(callNode->getArgOperand(0));
      uint32_t objId = AddressValue::getInternalID(newAddr);
      as.addHeapObject(objId);
      if (as.inVarToValTable(sizeId)) {
        IntervalValue size = as[sizeId].getInterval();
        if (size.is_numeral()) {
          as.setObjSize(objId,
                        static_cast<uint32_t>(size.getIntNumeralOrZero()));
        } else {
          as.setObjSize(objId, MaxFieldLimit);
        }
      } else {
        as.setObjSize(objId, MaxFieldLimit);
      }
    }
  };
  func_map["zmalloc"] = sse_zmalloc;

  // Additional allocation functions
  func_map["xcalloc"] = sse_calloc;
  func_map["xmalloc"] = sse_malloc;
  func_map["xrealloc"] = sse_realloc;
  func_map["safe_malloc"] = sse_malloc;
  func_map["safe_calloc"] = sse_calloc;
  func_map["safe_realloc"] = sse_realloc;

  // Memory allocation functions with size arguments
  auto sse_aligned_alloc = [this](const llvm::CallBase *callNode) {
    // aligned_alloc(alignment, size) - similar to malloc
    if (callNode->arg_size() < 2)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t lhsId = getValueId(callNode);
    uint32_t newAddr = AddressValue::getVirtualMemAddress(lhsId);
    AddressValue addr(newAddr);
    AbstractValue addrVal = addr;
    as[lhsId] = addrVal;
    if (callNode->arg_size() >= 2) {
      uint32_t sizeId =
          getValueId(callNode->getArgOperand(1)); // size is second arg
      uint32_t objId = AddressValue::getInternalID(newAddr);
      as.addHeapObject(objId);
      if (as.inVarToValTable(sizeId)) {
        IntervalValue size = as[sizeId].getInterval();
        if (size.is_numeral()) {
          as.setObjSize(objId,
                        static_cast<uint32_t>(size.getIntNumeralOrZero()));
        } else {
          as.setObjSize(objId, MaxFieldLimit);
        }
      } else {
        as.setObjSize(objId, MaxFieldLimit);
      }
    }
  };
  func_map["aligned_alloc"] = sse_aligned_alloc;
  func_map["memalign"] = sse_aligned_alloc;
  func_map["valloc"] = sse_malloc;
  func_map["pvalloc"] = sse_malloc;

  // posix_memalign - allocates in argument 0
  auto sse_posix_memalign = [this](const llvm::CallBase *callNode) {
    // int posix_memalign(void **memptr, size_t alignment, size_t size)
    // Stores result in argument 0
    if (callNode->arg_size() < 3)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);

    // Create a new address
    uint32_t lhsId = getValueId(callNode);
    uint32_t newAddr = AddressValue::getVirtualMemAddress(lhsId);
    AddressValue addr(newAddr);
    AbstractValue addrVal = addr;

    // Store to the pointer argument (arg 0)
    uint32_t ptrId = getValueId(callNode->getArgOperand(0));
    if (as.inVarToAddrsTable(ptrId)) {
      for (auto a : as[ptrId].getAddrs()) {
        as.store(a, addrVal);
      }
    }

    // Track object size
    uint32_t sizeId = getValueId(callNode->getArgOperand(2));
    uint32_t objId = AddressValue::getInternalID(newAddr);
    as.addHeapObject(objId);
    if (as.inVarToValTable(sizeId)) {
      IntervalValue size = as[sizeId].getInterval();
      if (size.is_numeral()) {
        as.setObjSize(objId, static_cast<uint32_t>(size.getIntNumeralOrZero()));
      } else {
        as.setObjSize(objId, MaxFieldLimit);
      }
    } else {
      as.setObjSize(objId, MaxFieldLimit);
    }
  };
  func_map["posix_memalign"] = sse_posix_memalign;

  // reallocarray - realloc with size check
  auto sse_reallocarray = [this](const llvm::CallBase *callNode) {
    // reallocarray(ptr, nmemb, size)
    if (callNode->arg_size() < 3)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t lhsId = getValueId(callNode);
    uint32_t newAddr = AddressValue::getVirtualMemAddress(lhsId);
    AddressValue addr(newAddr);
    AbstractValue addrVal = addr;
    as[lhsId] = addrVal;

    uint32_t nmembId = getValueId(callNode->getArgOperand(1));
    uint32_t sizeId = getValueId(callNode->getArgOperand(2));
    uint32_t objId = AddressValue::getInternalID(newAddr);
    as.addHeapObject(objId);

    IntervalValue nmemb(1, MaxFieldLimit);
    IntervalValue size(1, MaxFieldLimit);
    if (as.inVarToValTable(nmembId))
      nmemb = as[nmembId].getInterval();
    if (as.inVarToValTable(sizeId))
      size = as[sizeId].getInterval();

    int64_t totalSize =
        nmemb.ub().getIntNumeralOrZero() * size.ub().getIntNumeralOrZero();
    if (totalSize > 0 && totalSize <= static_cast<int64_t>(MaxFieldLimit)) {
      as.setObjSize(objId, static_cast<uint32_t>(totalSize));
    } else {
      as.setObjSize(objId, MaxFieldLimit);
    }
  };
  func_map["reallocarray"] = sse_reallocarray;

  // strdup/strndup
  auto sse_strdup = [this, sse_malloc](const llvm::CallBase *callNode) {
    // Similar to malloc - allocates and copies string
    if (callNode->arg_size() < 1)
      return;
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t lhsId = getValueId(callNode);
    uint32_t newAddr = AddressValue::getVirtualMemAddress(lhsId);
    AddressValue addr(newAddr);
    AbstractValue addrVal = addr;
    as[lhsId] = addrVal;
    // Track size if source is available
    if (callNode->arg_size() >= 1) {
      uint32_t srcId = getValueId(callNode->getArgOperand(0));
      uint32_t objId = AddressValue::getInternalID(newAddr);
      as.addHeapObject(objId);
      // Estimate size from strlen of source
      IntervalValue len = getStrlen(as, srcId);
      uint32_t elemSize = getElementByteSize(as, callNode->getArgOperand(0));
      uint64_t totalSize =
          static_cast<uint64_t>(len.ub().getIntNumeralOrZero()) + elemSize;
      as.setObjSize(
          objId,
          totalSize > MaxFieldLimit ? MaxFieldLimit
                                    : static_cast<uint32_t>(totalSize));
    } else {
      uint32_t objId = AddressValue::getInternalID(newAddr);
      as.addHeapObject(objId);
      as.setObjSize(objId, MaxFieldLimit);
    }
  };
  func_map["strdup"] = sse_strdup;
  func_map["strndup"] = sse_strdup;
  func_map["__strdup"] = sse_strdup;

  // String functions
  auto sse_strchr = [this](const llvm::CallBase *callNode) {
    // Returns pointer or NULL
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(IntervalValue::top());
  };
  func_map["strchr"] = sse_strchr;
  func_map["strrchr"] = sse_strchr;
  func_map["index"] = sse_strchr;
  func_map["rindex"] = sse_strchr;

  auto sse_strstr = [this](const llvm::CallBase *callNode) {
    // Returns pointer or NULL
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(IntervalValue::top());
  };
  func_map["strstr"] = sse_strstr;
  func_map["strcasestr"] = sse_strstr;

  // Time functions
  auto sse_time = [this](const llvm::CallBase *callNode) {
    // Returns time_t (typically integer)
    AbstractState &as = getAbsStateFromTrace(callNode);
    uint32_t lhsId = getValueId(callNode);
    as[lhsId] = AbstractValue(
        IntervalValue(0, 2000000000)); // Approximate Unix time range
  };
  func_map["time"] = sse_time;
  func_map["gmtime"] = sse_time;
  func_map["localtime"] = sse_time;
  func_map["ctime"] = sse_time;

  // String to number conversions
  auto sse_strtod = [this](const llvm::CallBase *callNode) {
    handleExtStrto(callNode);
  };
  func_map["strtod"] = sse_strtod;
  func_map["strtof"] = sse_strtod;
  func_map["strtold"] = sse_strtod;
  func_map["strtol"] = sse_strtod;
  func_map["strtoll"] = sse_strtod;
  func_map["strtoul"] = sse_strtod;
  func_map["strtoull"] = sse_strtod;
  func_map["strtol_l"] = sse_strtod;
  func_map["strtoll_l"] = sse_strtod;
}

void AEExtAPI::handleExtAPI(const llvm::CallBase *call,
                            const llvm::Function *resolvedCallee) {
  const llvm::Function *callee =
      resolvedCallee ? resolvedCallee : call->getCalledFunction();
  if (!callee)
    return;

  module_ = const_cast<llvm::Module *>(callee->getParent());
  std::string funcName = callee->getName().str();
  auto funcIt = func_map.find(funcName);
  if (funcIt != func_map.end()) {
    funcIt->second(call);
    return;
  }

  if (modelStdContainerCall(*this, call, funcName)) {
    return;
  }

  switch (getExtAPIType(callee)) {
  case MEMCPY:
    handleExtMemcpy(call);
    break;
  case MEMSET:
    handleExtMemset(call);
    break;
  case STRCPY:
    handleExtStrcpy(call);
    break;
  case STRCAT:
    handleExtStrcat(call);
    break;
  case ALLOC:
    handleExtAlloc(call);
    break;
  case REALLOC:
    handleExtRealloc(call);
    break;
  case FREE:
    handleExtFree(call);
    break;
  case STRLEN:
    handleExtStrlen(call);
    break;
  case SCANF:
    handleExtScanf(call);
    break;
  case SNPRINTF:
    handleExtSnprintf(call);
    break;
  case RECV:
    handleExtRecv(call);
    break;
  case ALLOC_ARG0:
    handleExtAllocArg0(call);
    break;
  case STRTOK:
    handleExtStrtok(call);
    break;
  case STRCHR:
    handleExtStrchr(call);
    break;
  case STRSTR:
    handleExtStrstr(call);
    break;
  case FGETS:
    handleExtFgets(call);
    break;
  case FREAD:
    handleExtFread(call);
    break;
  case TIME:
    handleExtTime(call);
    break;
  case ENV:
    handleExtEnv(call);
    break;
  case STRTO:
    handleExtStrto(call);
    break;
  case UNCLASSIFIED:
  default:
    {
      AbstractState &as = getAbsStateFromTrace(call);
      uint32_t lhsId = getValueId(call);
      as[lhsId] = AbstractValue(IntervalValue::top());
    }
    break;
  }
}

void AEExtAPI::handleExtMemcpy(const llvm::CallBase *call) {
  if (call->arg_size() < 3)
    return;
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t dstId = getValueId(call->getArgOperand(0));
  uint32_t srcId = getValueId(call->getArgOperand(1));
  uint32_t lenId = getValueId(call->getArgOperand(2));

  IntervalValue len(0, 4096);
  if (as.inVarToValTable(lenId)) {
    len = as[lenId].getInterval();
  }
  handleMemcpy(as, dstId, srcId, len, 0);

  uint32_t lhsId = getValueId(call);
  as[lhsId] = as[dstId];
}

void AEExtAPI::handleExtMemset(const llvm::CallBase *call) {
  if (call->arg_size() < 3)
    return;
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t dstId = getValueId(call->getArgOperand(0));
  uint32_t valId = getValueId(call->getArgOperand(1));
  uint32_t lenId = getValueId(call->getArgOperand(2));

  IntervalValue elem(0, 255);
  if (as.inVarToValTable(valId)) {
    elem = as[valId].getInterval();
  }
  IntervalValue len(0, 4096);
  if (as.inVarToValTable(lenId)) {
    len = as[lenId].getInterval();
  }
  handleMemset(as, dstId, elem, len);

  uint32_t lhsId = getValueId(call);
  as[lhsId] = as[dstId];
}

void AEExtAPI::handleExtStrcpy(const llvm::CallBase *call) {
  handleStrcpy(call);
  uint32_t lhsId = getValueId(call);
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t dstId = getValueId(call->getArgOperand(0));
  as[lhsId] = as[dstId];
}

void AEExtAPI::handleExtStrcat(const llvm::CallBase *call) {
  handleStrcat(call);
  uint32_t lhsId = getValueId(call);
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t dstId = getValueId(call->getArgOperand(0));
  as[lhsId] = as[dstId];
}

void AEExtAPI::handleExtAlloc(const llvm::CallBase *call) {
  if (call->arg_size() < 1)
    return;
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t lhsId = getValueId(call);
  uint32_t newAddr = AddressValue::getVirtualMemAddress(lhsId);
  as[lhsId] = AbstractValue(AddressValue(newAddr));

  uint32_t sizeId = getValueId(call->getArgOperand(0));
  uint32_t objId = AddressValue::getInternalID(newAddr);
  as.addHeapObject(objId);

  if (const auto *csize =
          llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0))) {
    as.setObjSize(objId, static_cast<uint32_t>(csize->getZExtValue()));
  } else if (as.inVarToValTable(sizeId)) {
    IntervalValue size = as[sizeId].getInterval();
    if (size.is_infinite()) {
      // Bounds are unbounded; getIntNumeral() would assert. Use conservative
      // limit.
      as.setObjSize(objId, MaxFieldLimit);
    } else if (size.is_numeral()) {
      as.setObjSize(objId, static_cast<uint32_t>(size.getIntNumeralOrZero()));
    } else {
      int64_t ub = size.ub().getIntNumeralOrZero();
      if (ub > 0 && ub <= static_cast<int64_t>(MaxFieldLimit)) {
        as.setObjSize(objId, static_cast<uint32_t>(ub));
      } else {
        as.setObjSize(objId, MaxFieldLimit);
      }
    }
  } else {
    as.setObjSize(objId, MaxFieldLimit);
  }
}

void AEExtAPI::handleExtRealloc(const llvm::CallBase *call) {
  if (call->arg_size() < 2)
    return;
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t lhsId = getValueId(call);
  uint32_t newAddr = AddressValue::getVirtualMemAddress(lhsId);
  as[lhsId] = AbstractValue(AddressValue(newAddr));

  uint32_t sizeId = getValueId(call->getArgOperand(1));
  uint32_t objId = AddressValue::getInternalID(newAddr);
  as.addHeapObject(objId);

  if (const auto *csize =
          llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1))) {
    as.setObjSize(objId, static_cast<uint32_t>(csize->getZExtValue()));
  } else if (as.inVarToValTable(sizeId)) {
    IntervalValue size = as[sizeId].getInterval();
    if (size.is_infinite()) {
      // Bounds are unbounded; getIntNumeral() would assert. Use conservative
      // limit.
      as.setObjSize(objId, MaxFieldLimit);
    } else if (size.is_numeral()) {
      as.setObjSize(objId, static_cast<uint32_t>(size.getIntNumeralOrZero()));
    } else {
      int64_t ub = size.ub().getIntNumeralOrZero();
      if (ub > 0 && ub <= static_cast<int64_t>(MaxFieldLimit)) {
        as.setObjSize(objId, static_cast<uint32_t>(ub));
      } else {
        as.setObjSize(objId, MaxFieldLimit);
      }
    }
  } else {
    as.setObjSize(objId, MaxFieldLimit);
  }
}

void AEExtAPI::handleExtFree(const llvm::CallBase *call) {
  if (call->arg_size() < 1)
    return;
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t freePtr = getValueId(call->getArgOperand(0));
  if (!as.inVarToAddrsTable(freePtr))
    return;
  for (auto addr : as[freePtr].getAddrs()) {
    if (!AbstractState::isInvalidMem(addr) && !AbstractState::isNullMem(addr)) {
      as.addToPendingFreedAddrs(addr);
    }
  }
}

void AEExtAPI::handleExtStrlen(const llvm::CallBase *call) {
  if (call->arg_size() < 1)
    return;
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t strId = getValueId(call->getArgOperand(0));
  IntervalValue len = getStrlen(as, strId);
  uint32_t lhsId = getValueId(call);
  as[lhsId] = AbstractValue(len);
}

void AEExtAPI::handleExtScanf(const llvm::CallBase *call) {
  if (call->arg_size() < 2)
    return;

  const llvm::Function *callee = call->getCalledFunction();
  if (!callee)
    return;

  AbstractState &as = getAbsStateFromTrace(call);

  // scanf/vscanf:         format @ arg0, outputs start at arg1
  // fscanf/sscanf family: format @ arg1, outputs start at arg2
  uint32_t formatIdx = 0;
  uint32_t dstStart = 1;
  const std::string calleeName = callee->getName().str();
  if (calleeName.find("fscanf") != std::string::npos ||
      calleeName.find("sscanf") != std::string::npos) {
    formatIdx = 1;
    dstStart = 2;
  }

  std::vector<IntegerSignedness> signednessHints;
  if (call->arg_size() > formatIdx) {
    std::string format;
    if (!tryExtractConstantString(call->getArgOperand(formatIdx), format)) {
      uint32_t formatId = getValueId(call->getArgOperand(formatIdx));
      format = strRead(as, formatId);
    }
    if (format != "???")
      signednessHints = parseScanfSignednessHints(format);
  }

  for (uint32_t i = dstStart; i < call->arg_size(); ++i) {
    const llvm::Value *dstOp = call->getArgOperand(i);
    if (!dstOp->getType()->isPointerTy())
      continue;

    uint32_t dstId = getValueId(dstOp);
    if (!as.inVarToAddrsTable(dstId))
      continue;

    llvm::Type *pointeeTy = dstOp->getType()->getPointerElementType();
    IntegerSignedness signHint = IntegerSignedness::Unknown;
    size_t hintIndex = i - dstStart;
    if (hintIndex < signednessHints.size())
      signHint = signednessHints[hintIndex];

    AbstractValue range(getTypeRangeWithHint(pointeeTy, signHint));
    for (auto addr : as[dstId].getAddrs()) {
      as.store(addr, range);
    }
  }
}

void AEExtAPI::handleExtSnprintf(const llvm::CallBase *call) {
  if (call->arg_size() < 3)
    return;
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t strId = getValueId(call->getArgOperand(0));
  uint32_t sizeId = getValueId(call->getArgOperand(1));

  IntervalValue maxSize(0, MaxFieldLimit);
  if (as.inVarToValTable(sizeId)) {
    maxSize = as[sizeId].getInterval();
    if (maxSize.ub().getIntNumeralOrZero() >
        static_cast<int64_t>(MaxFieldLimit)) {
      maxSize = IntervalValue(0, MaxFieldLimit);
    }
  }

  if (as.inVarToAddrsTable(strId)) {
    uint32_t maxLen = static_cast<uint32_t>(maxSize.ub().getIntNumeralOrZero());
    if (maxLen > 0 && maxLen < MaxFieldLimit) {
      AddressValue nullAddr =
          as.getGepObjAddrs(strId, IntervalValue(maxLen - 1));
      for (auto nullAddrVal : nullAddr) {
        as.store(nullAddrVal, AbstractValue(IntervalValue(0)));
      }
    }
  }

  uint32_t lhsId = getValueId(call);
  as[lhsId] = AbstractValue(maxSize);
}

void AEExtAPI::handleExtRecv(const llvm::CallBase *call) {
  if (call->arg_size() < 4)
    return;
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t lenId = getValueId(call->getArgOperand(2));

  constexpr uint32_t MaxRecvLen = 10000;
  IntervalValue len(0, MaxRecvLen);
  if (as.inVarToValTable(lenId)) {
    len = as[lenId].getInterval() - IntervalValue(1);
    if (len.lb().getIntNumeralOrZero() < 0) {
      len = IntervalValue(0, len.ub().getIntNumeralOrZero());
    }
  }

  uint32_t lhsId = getValueId(call);
  as[lhsId] = AbstractValue(len);
}

void AEExtAPI::handleStrcpy(const llvm::CallBase *call) {
  AbstractState &as = getAbsStateFromTrace(call);
  if (call->arg_size() < 2)
    return;

  uint32_t dstId = getValueId(call->getArgOperand(0));
  uint32_t srcId = getValueId(call->getArgOperand(1));

  if (!as.inVarToAddrsTable(dstId) || !as.inVarToAddrsTable(srcId))
    return;

  // Copy characters from src to dst until null terminator
  IntervalValue strLen = getStrlen(as, srcId);
  handleMemcpy(as, dstId, srcId, strLen, 0);

  // Add null terminator
  if (strLen.is_numeral()) {
    storeStringTerminator(as, dstId,
                          static_cast<uint32_t>(strLen.getIntNumeralOrZero()));
  }
}

void AEExtAPI::handleStrcat(const llvm::CallBase *call) {
  AbstractState &as = getAbsStateFromTrace(call);
  if (call->arg_size() < 2)
    return;

  uint32_t dstId = getValueId(call->getArgOperand(0));
  uint32_t srcId = getValueId(call->getArgOperand(1));

  if (!as.inVarToAddrsTable(dstId) || !as.inVarToAddrsTable(srcId))
    return;

  // Find end of dst string
  IntervalValue dstLen = getStrlen(as, dstId);
  IntervalValue srcLen = getStrlen(as, srcId);

  // Copy src to end of dst
  handleMemcpy(as, dstId, srcId, srcLen,
               static_cast<uint32_t>(dstLen.ub().getIntNumeralOrZero()));
  if (dstLen.is_numeral() && srcLen.is_numeral()) {
    storeStringTerminator(
        as, dstId,
        static_cast<uint32_t>(dstLen.getIntNumeralOrZero() +
                              srcLen.getIntNumeralOrZero()));
  }
}

IntervalValue AEExtAPI::getStrlen(AbstractState &as, uint32_t strId) {
  constexpr uint32_t MaxStrLen =
      10000; // Maximum string length to scan (matches MaxFieldLimit)

  if (!as.inVarToAddrsTable(strId)) {
    return IntervalValue(0, MaxStrLen);
  }

  const llvm::Value *strValue =
      AbstractInterpretation::getAEInstance().getValueFromIdStatic(strId);
  uint32_t elemSize = getElementByteSize(as, strValue);
  if (elemSize == 0)
    elemSize = 1;
  const std::pair<uint32_t, int64_t> strBase =
      resolvePointerBaseAndOffset(as, strValue, strId);
  const uint32_t baseStrId = strBase.first;
  const int64_t baseOffset = strBase.second;
  const uint32_t effectiveBaseOffset =
      static_cast<uint32_t>(std::max<int64_t>(0, baseOffset));

  // Get the object size to limit our search
  uint32_t maxSize = MaxStrLen;
  for (auto addr : as[baseStrId].getAddrs()) {
    uint32_t objId = as.getIDFromAddr(addr);
    uint32_t objSize = as.getObjSize(objId);
    if (objSize > effectiveBaseOffset) {
      uint32_t remaining = objSize - effectiveBaseOffset;
      if (remaining < maxSize)
        maxSize = remaining;
    }
  }

  // Scan memory byte by byte looking for null terminator
  uint32_t len = 0;
  bool foundNull = false;
  bool allNull = true;

  uint32_t maxElems = std::max<uint32_t>(1, maxSize / elemSize);
  for (uint32_t index = 0; index < maxElems && (index * elemSize) < MaxStrLen;
       ++index) {
    int64_t byteOffset = baseOffset + static_cast<int64_t>(index * elemSize);
    AddressValue gepAddrs =
        as.getGepObjAddrs(baseStrId, IntervalValue(byteOffset));
    AbstractValue val;
    bool hasValue = false;

    // Load value at this offset
    for (auto addr : gepAddrs) {
      uint32_t objId = as.getIDFromAddr(addr);
      if (as.inAddrToValTable(objId)) {
        if (!hasValue) {
          val = as.load(addr);
          hasValue = true;
        } else {
          val.join_with(as.load(addr));
        }
      }
    }

    if (!hasValue) {
      // No value stored - treat as unknown/uninitialized.
      // Conservatively return a length range later rather than a fixed 0.
      allNull = false;
      break;
    }

    // Check if this is a null terminator
    if (val.isInterval()) {
      IntervalValue interval = val.getInterval();
      if (interval.is_numeral() && interval.getIntNumeralOrZero() == 0) {
        foundNull = true;
        break;
      } else if (!interval.contains(0)) {
        // Definitely not null
        allNull = false;
        len = (index + 1) * elemSize;
      } else {
        // Might be null, might not
        allNull = false;
        len = (index + 1) * elemSize;
      }
    } else {
      // Not an interval value - assume non-null
      allNull = false;
      len = (index + 1) * elemSize;
    }
  }

  if (!foundNull && !allNull) {
    // Found non-null characters but no null terminator
    // Return conservative estimate: [len, maxSize]
    return IntervalValue(len, maxSize);
  } else if (foundNull) {
    // Found null terminator at position len
    return IntervalValue(len, len);
  } else {
    // All null or no information - return conservative estimate
    return IntervalValue(0, maxSize);
  }
}

void AEExtAPI::handleMemcpy(AbstractState &as, uint32_t dstId, uint32_t srcId,
                            IntervalValue len, uint32_t start_idx) {
  if (!as.inVarToAddrsTable(dstId) || !as.inVarToAddrsTable(srcId))
    return;

  const llvm::Value *dstValue =
      AbstractInterpretation::getAEInstance().getValueFromIdStatic(dstId);
  const llvm::Value *srcValue =
      AbstractInterpretation::getAEInstance().getValueFromIdStatic(srcId);
  uint32_t elemSize = getElementByteSize(as, dstValue);
  if (elemSize == 0)
    elemSize = 1;
  const std::pair<uint32_t, int64_t> dstBase =
      resolvePointerBaseAndOffset(as, dstValue, dstId);
  const std::pair<uint32_t, int64_t> srcBase =
      resolvePointerBaseAndOffset(as, srcValue, srcId);
  const uint32_t baseDstId = dstBase.first;
  const int64_t baseDstOffset = dstBase.second;
  const uint32_t baseSrcId = srcBase.first;
  const int64_t baseSrcOffset = srcBase.second;

  uint32_t sizeBytes = getTrackedByteCount(len, /*useLowerBound=*/true);
  uint32_t numElements =
      std::min<uint32_t>(MaxFieldLimit, sizeBytes / elemSize);

  // Copy byte by byte (or element by element)
  for (uint32_t i = 0; i < numElements; ++i) {
    int64_t srcOffset = baseSrcOffset + static_cast<int64_t>(i * elemSize);
    int64_t dstOffset =
        baseDstOffset + static_cast<int64_t>(start_idx) +
        static_cast<int64_t>(i * elemSize);

    // Get source addresses at this offset
    AddressValue srcAddrs =
        as.getGepObjAddrs(baseSrcId, IntervalValue(srcOffset));

    // Get destination addresses at this offset
    AddressValue dstAddrs =
        as.getGepObjAddrs(baseDstId, IntervalValue(dstOffset));

    // Join all values from source addresses
    AbstractValue srcVal;
    bool hasSrcVal = false;

    for (auto srcAddr : srcAddrs) {
      if (!AddressValue::isVirtualMemAddress(srcAddr))
        continue;
      uint32_t srcObjId = as.getIDFromAddr(srcAddr);
      if (as.inAddrToValTable(srcObjId)) {
        if (!hasSrcVal) {
          srcVal = as.load(srcAddr);
          hasSrcVal = true;
        } else {
          srcVal.join_with(as.load(srcAddr));
        }
      } else if (as.inAddrToAddrsTable(srcObjId)) {
        // Source is a pointer - copy the pointer value
        if (!hasSrcVal) {
          srcVal = as.load(srcAddr);
          hasSrcVal = true;
        } else {
          srcVal.join_with(as.load(srcAddr));
        }
      }
    }

    // If no source value found, use top (unknown value)
    if (!hasSrcVal) {
      srcVal = AbstractValue(IntervalValue::top());
    }

    // Store to all destination addresses
    for (auto dstAddr : dstAddrs) {
      as.store(dstAddr, srcVal);
    }
  }
}

void AEExtAPI::handleMemset(AbstractState &as, uint32_t dstId,
                            IntervalValue elem, IntervalValue len) {
  if (!as.inVarToAddrsTable(dstId))
    return;

  const llvm::Value *dstValue =
      AbstractInterpretation::getAEInstance().getValueFromIdStatic(dstId);
  uint32_t elemSize = getElementByteSize(as, dstValue);
  if (elemSize == 0)
    elemSize = 1;
  const std::pair<uint32_t, int64_t> dstBase =
      resolvePointerBaseAndOffset(as, dstValue, dstId);
  const uint32_t baseDstId = dstBase.first;
  const int64_t baseDstOffset = dstBase.second;

  uint32_t sizeBytes = getTrackedByteCount(len, /*useLowerBound=*/true);
  uint32_t numElements =
      std::min<uint32_t>(MaxFieldLimit, sizeBytes / elemSize);

  AbstractValue val(elem);

  // Set each byte (or element) to the element value
  for (uint32_t i = 0; i < numElements; ++i) {
    AddressValue dstAddrs = as.getGepObjAddrs(
        baseDstId,
        IntervalValue(baseDstOffset + static_cast<int64_t>(i * elemSize)));

    // Set all destination addresses at this offset to the element value
    for (auto dstAddr : dstAddrs) {
      if (!AddressValue::isVirtualMemAddress(dstAddr))
        continue;
      // Load existing value and join with element (for precision)
      if (as.inAddrToValTable(as.getIDFromAddr(dstAddr))) {
        AbstractValue existing = as.load(dstAddr);
        existing.join_with(val);
        as.store(dstAddr, existing);
      } else {
        as.store(dstAddr, val);
      }
    }
  }
}

IntervalValue AEExtAPI::getRangeLimitFromType(llvm::Type *type) {
  return getTypeRangeWithHint(type, IntegerSignedness::Signed);
}

AbstractState &AEExtAPI::getAbsStateFromTrace(const llvm::Instruction *val) {
  auto it = abstractTrace.find(val);
  if (it == abstractTrace.end()) {
    llvm::report_fatal_error(
        "No abstract state in trace for external API call");
  }
  return it->second;
}

std::string AEExtAPI::strRead(AbstractState &as, uint32_t strId) {
  if (!as.inVarToAddrsTable(strId)) {
    return "???";
  }

  std::string result;
  constexpr uint32_t MaxStrLen = 10000;
  uint32_t maxSize = MaxStrLen;

  // Get object size to limit search
  for (auto addr : as[strId].getAddrs()) {
    uint32_t objId = as.getIDFromAddr(addr);
    uint32_t objSize = as.getObjSize(objId);
    if (objSize > 0 && objSize < maxSize) {
      maxSize = objSize;
    }
  }

  // Scan memory byte by byte for null-terminated string
  for (uint32_t index = 0; index < maxSize && index < MaxStrLen; ++index) {
    AddressValue gepAddrs = as.getGepObjAddrs(strId, IntervalValue(index));
    AbstractValue val;
    bool hasValue = false;

    // Load value at this offset
    for (auto addr : gepAddrs) {
      uint32_t objId = as.getIDFromAddr(addr);
      if (as.inAddrToValTable(objId)) {
        if (!hasValue) {
          val = as.load(addr);
          hasValue = true;
        } else {
          val.join_with(as.load(addr));
        }
      }
    }

    if (!hasValue) {
      // No value - might be null terminator or uninitialized
      break;
    }

    // Check if this is a null terminator
    if (val.isInterval()) {
      IntervalValue interval = val.getInterval();
      if (interval.is_numeral()) {
        int64_t charVal = interval.getIntNumeralOrZero();
        if (charVal == 0) {
          // Null terminator found
          break;
        }
        if (charVal >= 0 && charVal <= 127) {
          // Valid ASCII character
          result += static_cast<char>(charVal);
        } else {
          // Non-ASCII or out of range
          result += "?";
        }
      } else {
        // Interval value - can't determine exact character
        result += "?";
        // Continue but mark as uncertain
      }
    } else {
      // Not an interval - can't read as character
      result += "?";
    }
  }

  return result.empty() ? "???" : result;
}

uint32_t AEExtAPI::getValueId(const llvm::Value *val) {
  return AbstractInterpretation::getValueIdStatic(val);
}

void AEExtAPI::handleExtAllocArg0(const llvm::CallBase *call) {
  // Allocates and stores result in argument 0 (e.g., asprintf, posix_memalign)
  // This is similar to ALLOC but the result is stored in an argument
  if (call->arg_size() < 1)
    return;

  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t ptrId = getValueId(call->getArgOperand(0));

  if (!as.inVarToAddrsTable(ptrId))
    return;

  uint32_t freshObjId = getValueId(call);
  uint32_t newAddr = AddressValue::getVirtualMemAddress(freshObjId);
  AbstractValue addrVal{AddressValue(newAddr)};
  uint32_t objId = AddressValue::getInternalID(newAddr);
  as.addHeapObject(objId);

  for (uint32_t slotAddr : as[ptrId].getAddrs())
    as.store(slotAddr, addrVal);

  uint32_t sizeArgIndex = UINT32_MAX;
  if (const llvm::Function *callee = call->getCalledFunction()) {
    if (callee->getName() == "posix_memalign" && call->arg_size() >= 3)
      sizeArgIndex = 2;
  }

  if (sizeArgIndex != UINT32_MAX) {
    uint32_t sizeId = getValueId(call->getArgOperand(sizeArgIndex));
    if (as.inVarToValTable(sizeId)) {
      IntervalValue size = as[sizeId].getInterval();
      if (size.is_numeral()) {
        as.setObjSize(objId, static_cast<uint32_t>(size.getIntNumeralOrZero()));
        return;
      }
    }
  }
  as.setObjSize(objId, MaxFieldLimit);
}

void AEExtAPI::handleExtStrtok(const llvm::CallBase *call) {
  // String tokenization - returns pointer to token or NULL
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t lhsId = getValueId(call);

  // Conservative: return TOP (any possible pointer)
  as[lhsId] = AbstractValue(IntervalValue::top());
}

void AEExtAPI::handleExtStrchr(const llvm::CallBase *call) {
  // String character search - returns pointer to first/last occurrence or NULL
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t lhsId = getValueId(call);

  // Conservative: return TOP (any possible pointer)
  as[lhsId] = AbstractValue(IntervalValue::top());
}

void AEExtAPI::handleExtStrstr(const llvm::CallBase *call) {
  // String substring search - returns pointer to substring or NULL
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t lhsId = getValueId(call);

  // Conservative: return TOP (any possible pointer)
  as[lhsId] = AbstractValue(IntervalValue::top());
}

void AEExtAPI::handleExtFgets(const llvm::CallBase *call) {
  // fgets - reads string from file, returns pointer or NULL
  if (call->arg_size() < 2)
    return;

  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t dstId = getValueId(call->getArgOperand(0));

  // If destination exists, model reading a string
  if (as.inVarToAddrsTable(dstId)) {
    // Conservative: assume string length is [0, MaxFieldLimit]
    // Store null terminator at some position
    for (auto addr : as[dstId].getAddrs()) {
      uint32_t objId = as.getIDFromAddr(addr);
      uint32_t objSize = as.getObjSize(objId);
      if (objSize > 0) {
        // Store null terminator at the end
        AddressValue nullAddr =
            as.getGepObjAddrs(dstId, IntervalValue(objSize - 1));
        for (auto na : nullAddr) {
          as.store(na, AbstractValue(IntervalValue(0)));
        }
      }
    }
  }

  // Return value is the string pointer (or NULL)
  uint32_t lhsId = getValueId(call);
  as[lhsId] = as[dstId];
}

void AEExtAPI::handleExtFread(const llvm::CallBase *call) {
  // fread - already handled in initExtFunMap
  // This is a placeholder for additional fread handling if needed
  (void)call; // Unused
}

void AEExtAPI::handleExtTime(const llvm::CallBase *call) {
  // Time functions - return pointer to struct tm or time_t
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t lhsId = getValueId(call);

  // Conservative: return TOP
  as[lhsId] = AbstractValue(IntervalValue::top());
}

void AEExtAPI::handleExtEnv(const llvm::CallBase *call) {
  // Environment functions - getenv returns pointer to string or NULL
  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t lhsId = getValueId(call);

  // Conservative: return TOP
  as[lhsId] = AbstractValue(IntervalValue::top());
}

void AEExtAPI::handleExtStrto(const llvm::CallBase *call) {
  // String to number conversion - strtod, strtol, etc.
  if (call->arg_size() < 1)
    return;

  AbstractState &as = getAbsStateFromTrace(call);
  uint32_t lhsId = getValueId(call);

  // Get the result type and return appropriate range
  llvm::Type *retType = call->getType();
  IntegerSignedness signHint = IntegerSignedness::Unknown;
  if (const llvm::Function *callee = call->getCalledFunction())
    signHint = getStrtoSignedness(callee->getName());
  as[lhsId] = AbstractValue(getTypeRangeWithHint(retType, signHint));
}

} // namespace analysis
} // namespace lotus
