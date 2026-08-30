#include "Solvers/Datalog/Frontend/Frontend.h"

#include <cctype>
#include <stdexcept>
#include <string>

#include <llvm/ADT/StringRef.h>

namespace lotus::datalog::frontend {
namespace {

InputFormat detectFormat(llvm::StringRef input) {
  std::size_t offset = 0;
  while (offset < input.size()) {
    if (std::isspace(static_cast<unsigned char>(input[offset]))) {
      ++offset;
      continue;
    }
    if (input[offset] == ';') {
      while (offset < input.size() && input[offset] != '\n')
        ++offset;
      continue;
    }
    break;
  }
  if (offset == input.size())
    throw std::invalid_argument("input program is empty");
  if (input[offset] == '{')
    return InputFormat::Json;
  if (input[offset] == '(')
    return InputFormat::Z3;
  return InputFormat::Datalog;
}

} // namespace

FrontendError::FrontendError(std::string code, std::string source,
                             std::size_t line, std::size_t column,
                             std::string message)
    : std::invalid_argument((source.empty() ? std::string("<input>") : source) +
                            ":" + std::to_string(line) + ":" +
                            std::to_string(column) + ": " + message),
      code_(std::move(code)), source_(std::move(source)), line_(line),
      column_(column) {}

InputFormat parseInputFormat(llvm::StringRef name) {
  if (name == "auto")
    return InputFormat::Auto;
  if (name == "json")
    return InputFormat::Json;
  if (name == "datalog" || name == "dl")
    return InputFormat::Datalog;
  if (name == "z3" || name == "smt2")
    return InputFormat::Z3;
  throw std::invalid_argument("unknown input format '" + name.str() + "'");
}

void executeInput(llvm::StringRef input, InputFormat format,
                  const RunOptions &options, llvm::raw_ostream &output) {
  const SourceUnit source{"<input>", input};
  executeInputs(source, format, options, output);
}

void executeInputs(llvm::ArrayRef<SourceUnit> inputs, InputFormat format,
                   const RunOptions &options, llvm::raw_ostream &output) {
  if (inputs.empty())
    throw std::invalid_argument("no input sources were provided");
  if (format == InputFormat::Auto)
    format = detectFormat(inputs.front().content);
  if (format == InputFormat::Json) {
    if (inputs.size() != 1)
      throw std::invalid_argument("JSON input accepts exactly one source");
    internal::execute(internal::parseJson(inputs.front()), options, output);
    return;
  }
  internal::FrontendIR program = format == InputFormat::Datalog
                                     ? internal::parseDatalog(
                                           inputs, options.source_resolver)
                                     : internal::parseZ3(inputs);
  internal::execute(program, options, output);
}

} // namespace lotus::datalog::frontend
