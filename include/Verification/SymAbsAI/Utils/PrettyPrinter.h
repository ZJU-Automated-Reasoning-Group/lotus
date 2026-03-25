#pragma once

#include "Verification/SymAbsAI/Utils/Utils.h"

#include <map>
#include <sstream>
#include <type_traits>

namespace z3 {
class expr;
} // namespace z3

namespace symabs_ai {
namespace pp {
/**
 * Wrapper class for emitting TeX output in a PrettyPrinter.
 */
class tex {
private:
  friend class ::symabs_ai::PrettyPrinter;
  std::string Plaintext_, Tex_;

public:
  /**
   * Creates a new TeX object with a given `plaintext` and `tex`
   * representation.
   */
  tex(const char *plaintext, const char *tex)
      : Plaintext_(plaintext), Tex_(tex) {}
};

extern tex top;
extern tex bottom;
extern tex rightarrow;
extern tex in;
} // namespace pp

/**
 * Aids in pretty printing of abstract values.
 *
 * An implementation of AbstractValue::prettyPrint() receives an instance of
 * this class and may use the `<<` operator to output various objects in a
 * way that supports both textual output to the terminal and HTML.
 */
class PrettyPrinter {
public:
  class Entry {
  private:
    PrettyPrinter *PP_;
    Entry(const Entry &) = delete;
    Entry &operator=(const Entry &) = delete;

  public:
    Entry(PrettyPrinter *pp, const std::string &class_name = "");
    ~Entry();
  };

private:
  bool OutputHTML_;
  std::ostringstream Result_;

public:
  /**
   * Creates a new instance of this class.
   *
   * If `output_html` is true, then the class will produce HTML output.
   * Otherwise, it will emit plain text and try to represent all the special
   * constructs (TeX, formulas, etc.) in a possibly readable way.
   */
  PrettyPrinter(bool output_html) : OutputHTML_(output_html) {}

  /**
   * Outputs a Z3 expression as a formula.
   */
  void outputFormula(const z3::expr &expr,
                     const std::map<std::string, llvm::Value *> &var_map);

  /**
   * Outputs a plain string.
   */
  PrettyPrinter &operator<<(const std::string &x);

  /**
   * Outputs a special TeX value.
   *
   * Examples:
   *    pretty_printer << pp::rightarrow;
   *    pretty_printer << pp::tex("N", "\\mathfrak{N}");
   */
  PrettyPrinter &operator<<(const pp::tex &);

  /**
   * Pretty-prints an LLVM Value.
   */
  PrettyPrinter &operator<<(const llvm::Value *value);
  PrettyPrinter &operator<<(const llvm::Value &value) {
    return *this << &value;
  }

  /**
   * Outputs a number.
   */
  template <typename T, typename = typename std::enable_if<
                            std::is_arithmetic<T>::value>::type>
  PrettyPrinter &operator<<(T x) {
    Result_ << x;
    return *this;
  }

  /**
   * Returns the string representation of the output.
   */
  std::string str() { return Result_.str(); }

  /**
   * If true, then pretty-printing of compound values (like Product) can
   * omit some of the subcomponents if they're top or the whole abstract
   * value is equal to bottom.
   *
   * If false, the implementation of prettyPrint() for a compound abstract
   * value should always call prettyPrint() for all the components with this
   * object as an argument. This is necessary for verifying some of the test
   * outputs in plain text mode.
   */
  bool compactProducts() { return OutputHTML_; }
};

/**
 * Helper class for producing output suitable for processing with annotate.py
 *
 * Outputs to a given stream a collection of annotations for a given function
 * in the form of a JSON object. Call one of the emit() methods to output an
 * annotation.
 *
 * This class will write to the output stream `out` in its constructor *and*
 * destructor so pay attention to the scope if you want to use it multiple
 * times or if you are passing a local stream.
 */
class JsonAnnotationOutput {
private:
  std::ostream *Out_;
  bool NeedsComma_ = false;

public:
  JsonAnnotationOutput(std::ostream *out, const llvm::Function *func);
  void emit(const std::string &annotation, int line, int col = -1);
  void emit(const AbstractValue *aval, int line, int col = -1);
  ~JsonAnnotationOutput();
};
}; // namespace symabs_ai
