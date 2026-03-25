/**
 * A template-based, LLVM-enabled pretty-printer.
 *
 * Full sources and license information available on the
 * <a href="http://github.com/tdudziak/repr">project website</a>.
 */

#ifndef REPR_HPP
#define REPR_HPP

#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <sstream>
#include <type_traits>
#include <vector>

// Enable LLVM-aware pretty-printing in this integration.
// The original library used LLVM_C_CORE_H to auto-detect inclusion via the C
// API, but in Lotus/SymAbsAI we always have access to the C++ IR
// headers, so we unconditionally enable the LLVM support here.
#ifndef ENABLE_REPR_LLVM
#define ENABLE_REPR_LLVM 1
#endif

#ifdef ENABLE_REPR_LLVM
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>
#endif

// forward declaration
namespace repr_impl {
template <typename T> void repr_stream(std::ostream &, const T &);
} // namespace repr_impl

template <typename T> std::string repr(const T &x) {
  std::ostringstream out;
  repr_impl::repr_stream(out, x);

  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  std::string result = out.str();
  result.erase(result.begin(),
               std::find_if(result.begin(), result.end(), not_space));
  result.erase(std::find_if(result.rbegin(), result.rend(), not_space).base(),
               result.end());

  return result;
}

namespace repr_impl {
using std::enable_if;
using std::is_function;
using std::is_same;
using std::remove_cv;
using std::remove_extent;
using std::remove_reference;

#ifdef ENABLE_REPR_LLVM
struct debug_info {
  std::string name = "";
  std::string file = "";
  int line_number = -1;
  int column_number = -1;
};

inline void find_debug_info(const llvm::Value &val, debug_info *dinfo) {
  using namespace llvm;
  auto *instr_ptr = dyn_cast<Instruction>(&val);

#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR >= 6
  MDNode *mdata = instr_ptr ? instr_ptr->getMetadata("dbg") : nullptr;

#if LLVM_VERSION_MINOR == 6
  DILocation diloc_val(mdata);
  DILocation *dloc = &diloc_val;
#else
  DILocation *dloc = dyn_cast_or_null<DILocation>(mdata);
#endif

  if (dloc) {
#if LLVM_VERSION_MINOR == 6
    dinfo->line_number = dloc->getLineNumber();
    dinfo->column_number = dloc->getColumnNumber();
    DIScope discope_val(dloc->getScope());
    DIScope *discope = &discope_val;
#else
    dinfo->line_number = dloc->getLine();
    dinfo->column_number = dloc->getColumn();
    DIScope *discope = dyn_cast_or_null<DIScope>(dloc->getScope());
#endif

    if (discope != nullptr) {
      std::string fname = discope->getFilename().str();
      if (fname.size() > 0)
        dinfo->file = std::move(fname);
    }
  }
#endif

  // to find the variable name try to locate a call to llvm.dbg.value in the
  // same BB with this value as argument
  if (instr_ptr != nullptr) {
    auto *bb = instr_ptr->getParent();
    for (auto &other_instr : *bb) {
      if (auto *as_call = dyn_cast<CallInst>(&other_instr)) {
        auto *func = as_call->getCalledFunction();

        if (func == nullptr || func->getName().str() != "llvm.dbg.value")
          continue;

        std::string name;

#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR <= 5
        // LLVM <= 3.5 has metadata within the same hierarchy as Values
        MDNode *var = dyn_cast<MDNode>(as_call->getOperand(0));
        if (var->getOperand(0) != instr_ptr)
          continue;

        DIVariable divar(dyn_cast<MDNode>(as_call->getOperand(2)));
        name = divar.getName().str();
#else
        auto *meta0 =
            dyn_cast<MetadataAsValue>(as_call->getOperand(0))->getMetadata();

        if (meta0) {
          auto *as_vam = dyn_cast<ValueAsMetadata>(meta0);
          if (as_vam && as_vam->getValue() != instr_ptr)
            continue;
        }

        auto *meta2 =
            dyn_cast<MetadataAsValue>(as_call->getOperand(2))->getMetadata();

#if LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR == 6
        DIVariable divar(dyn_cast<MDNode>(meta2));
        name = divar.getName().str();
#else
        // from LLVM 3.7 onwards DIVariable inherits from MDNode
        auto *divar = dyn_cast_or_null<DIVariable>(meta2);
        if (divar)
          name = divar->getName().str();
#endif
#endif

        if (name.size() > 0)
          dinfo->name = std::move(name);
      }
    }
  }
}

// Will print `(<name>@?<filename>:?<line_number>?)` depending on what
// information is available, or do nothing if nothing is known.
inline void repr_debug_loc(std::ostream &out, const llvm::Value &val) {
  using namespace llvm;
  debug_info dinfo;

  if (const auto *bb_ptr = dyn_cast<BasicBlock>(&val)) {
    // find the first instruction with debug info
    for (auto &inst : *bb_ptr) {
      find_debug_info(inst, &dinfo);
      if (dinfo.line_number > 0)
        break;
    }

    // the name found is not the name of the basic block
    dinfo.name = "";
  } else {
    find_debug_info(val, &dinfo);
  }

  bool has_name = dinfo.name.size() > 0;
  bool has_file = dinfo.file.size() > 0;
  bool has_line_number = dinfo.line_number > 0;
  std::string delayed_out = "";

  if (!has_name && !has_file && !has_line_number)
    return;

  out << "(";

  if (has_name) {
    out << dinfo.name;
    delayed_out = "@";
  }

  if (has_file) {
    out << delayed_out << dinfo.file;
    delayed_out = ":";
  }

  if (has_line_number)
    out << delayed_out << dinfo.line_number;

  out << ")";
}
#endif

/**
 * Dummy struct used to resolve ambiguous function overloads.
 *
 * If `overload_priority<0>()` is passed as an argument to a function, the
 * overloads with a formal argument of type `overload_priority<n>` will be
 * preferred for the smallest `n`. This helps to reduce guards in SFINAE-like
 * template hacks.
 */
template <int n> struct overload_priority : public overload_priority<n + 1> {};

template <> struct overload_priority<100> {};

template <typename T, int n> struct tuple_repr {
  void operator()(const T &tuple, std::vector<std::string> *out) {
    tuple_repr<T, n - 1>()(tuple, out);
    out->push_back(repr(std::get<n - 1>(tuple)));
  }
};

template <typename T> struct tuple_repr<T, 0> {
  void operator()(const T &, std::vector<std::string> *) {}
};

/**
 * Trait for testing whether a type is a C-style string or std::string.
 */
template <typename T> struct is_string_like {
  static const bool value =
      is_same<T, char *>::value || is_same<T, const char *>::value ||
      is_same<typename remove_extent<T>::type, char>::value ||
      is_same<typename remove_extent<T>::type, const char>::value ||
      is_same<T, std::string>::value;
};

/**
 * Unimplemented function that provides a value of a given type.
 *
 * This function cannot be called but it can be used in `decltype(...)` to get
 * a type of expressions using values of a given type.
 */
template <typename T> T val();

// function type (NOT std::function)
// Has to come before pointers as function types are infinitely-dereferencable
// pointer-like things.
template <typename T,
          typename = typename enable_if<is_function<T>::value>::type>
void repr_stream(std::ostream &out, const T &x, overload_priority<0>) {
  out << "<function@" << &x << ">";
}

// string-like: char*, const char*, char[], and std::string
template <typename T,
          typename = typename enable_if<is_string_like<T>::value>::type>
void repr_stream(std::ostream &out, const T &x, overload_priority<1>) {
  out << "\"" << x << "\"";
}

// pointers dumb and smart
template <typename T, typename = decltype(*val<T>()),
          typename = decltype(!val<T>())>
void repr_stream(std::ostream &out, const T &x, overload_priority<2>) {
  if (!x)
    out << "nullptr"; // also includes some "false" iterators
  else
    out << repr(*x);
}

// iterators and smart pointers that don't support conversions to bool
template <typename T, typename = decltype(*val<T>())>
void repr_stream(std::ostream &out, const T &x, overload_priority<3>) {
  out << repr(*x);
}

#ifdef ENABLE_REPR_LLVM
// all LLVM values
template <typename T, typename = decltype(repr_debug_loc(val<std::ostream &>(),
                                                         val<T &>()))>
void repr_stream(std::ostream &out, const T &x, overload_priority<4>) {
  std::string name = x.getName().str();

  if (name.size() > 0) {
    out << name;
  } else {
    std::string result;
    llvm::raw_string_ostream raw(result);
    raw << x;
    out << result;
  }

  repr_debug_loc(out, x);
}
#endif

// tuples and tuple-like things like std::pair and std::array
template <typename T, typename = decltype(std::get<0>(val<T>()))>
void repr_stream(std::ostream &out, const T &x, overload_priority<5>) {
  tuple_repr<T, std::tuple_size<T>::value> tr;
  std::vector<std::string> sub_reprs;
  tr(x, &sub_reprs);

  out << "(";
  bool needs_comma = false;
  for (auto &x : sub_reprs) {
    if (needs_comma)
      out << ", ";

    out << x;
    needs_comma = true;
  }
  out << ")";
}

// ostream-printable
template <typename T, typename = decltype(std::cout << val<T>())>
void repr_stream(std::ostream &out, const T &x, overload_priority<6>) {
  std::ios::fmtflags oldflags(out.flags());
  out << std::boolalpha << x;
  out.flags(oldflags);
}

// iterable (container) of pairs; print like a map
template <typename T, typename = decltype(val<T>().begin()->first),
          typename = decltype(val<T>().begin()->second)>
void repr_stream(std::ostream &out, const T &xs, overload_priority<7>) {
  bool needs_comma = false;
  out << "{";

  for (const auto &x : xs) {
    if (needs_comma)
      out << ", ";

    out << repr(x.first) << ": " << repr(x.second);
    needs_comma = true;
  }

  out << "}";
}

// iterable (container) of chars; print like a string
// FIXME: ugly blob of template magic
template <typename T,
          typename = typename enable_if<is_same<
              char, typename remove_cv<typename remove_reference<decltype(*(
                        val<T>().begin()))>::type>::type>::value>::type>
void repr_stream(std::ostream &out, const T &xs, overload_priority<8>) {
  out << "\"";
  for (auto x : xs) {
    out << x;
  }
  out << "\"";
}

// iterable
template <typename T, typename = decltype(val<T>().begin())>
void repr_stream(std::ostream &out, const T &xs, overload_priority<9>) {
  bool needs_brackets = false;
  std::vector<std::string> contents;

  for (const auto &x : xs) {
    std::string repr_x = repr(x);

    bool x_needs_brackets = false;
    for (char c : repr_x) {
      if (std::isspace(c) || c == ',')
        x_needs_brackets = true;
    }

    // bracketing possibly not needed if contents are already delimited
    if (repr_x.size() >= 2) {
      char a = repr_x[0];
      char b = repr_x[repr_x.size() - 1];

      if (a == '{' && b == '}')
        x_needs_brackets = false;

      if (a == '[' && b == ']')
        x_needs_brackets = false;
    }

    needs_brackets = needs_brackets || x_needs_brackets;
    contents.push_back(std::move(repr_x));
  }

  bool needs_comma = false;
  out << "[";
  for (const std::string &x : contents) {
    if (needs_comma)
      out << ", ";

    if (needs_brackets)
      out << "<" << x << ">";
    else
      out << x;

    needs_comma = true;
  }
  out << "]";
}

#ifdef ENABLE_REPR_LLVM
// other LLVM objects that can be printed to a raw_ostream
template <typename T,
          typename = decltype(val<llvm::raw_ostream &>() << val<T>())>
void repr_stream(std::ostream &out, const T &x, overload_priority<10>) {
  std::string result;
  llvm::raw_string_ostream raw(result);
  raw << x;
  out << result;
}
#endif

// other: just print the address
template <typename T>
void repr_stream(std::ostream &out, const T &x, overload_priority<100>) {
  out << "<" << &x << ">";
}

// dispatch to one of the overloads above
template <typename T> void repr_stream(std::ostream &out, const T &x) {
  repr_stream(out, x, overload_priority<0>());
}
} // namespace repr_impl

#endif
