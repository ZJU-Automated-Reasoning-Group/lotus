#pragma once

#include "Solvers/Datalog/Runtime/Scheduler.h"

#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus::datalog::frontend {

struct OwnedSourceUnit {
  std::string name;
  std::string content;
};

using SourceResolver = std::function<std::optional<OwnedSourceUnit>(
    llvm::StringRef including_source, llvm::StringRef requested_path)>;

struct RunOptions {
  ExecutionOptions execution;
  // Optional import policy for native `.include "path"` directives. The
  // frontend itself performs no filesystem access.
  SourceResolver source_resolver;
  bool validate_only = false;
  bool explain = false;
  bool explain_analyze = false;
  bool pretty = false;
};

struct SourceUnit {
  // Both strings are borrowed for the duration of the frontend call. Source
  // order controls rule/fact insertion order, but declarations may appear in
  // any unit because name resolution happens after all sources are parsed.
  llvm::StringRef name;
  llvm::StringRef content;
};

class FrontendError : public std::invalid_argument {
public:
  FrontendError(std::string code, std::string source, std::size_t line,
                std::size_t column, std::string message);

  const std::string &code() const { return code_; }
  const std::string &source() const { return source_; }
  std::size_t line() const { return line_; }
  std::size_t column() const { return column_; }

private:
  std::string code_;
  std::string source_;
  std::size_t line_ = 1;
  std::size_t column_ = 1;
};

enum class InputFormat {
  Auto,
  Json,
  Datalog,
  Z3,
};

InputFormat parseInputFormat(llvm::StringRef name);

void executeJson(llvm::StringRef input, const RunOptions &options,
                 llvm::raw_ostream &output);
std::string translateDatalogToJson(llvm::StringRef input);
std::string translateDatalogToJson(llvm::ArrayRef<SourceUnit> inputs);
std::string translateZ3ToJson(llvm::StringRef input);
std::string translateZ3ToJson(llvm::ArrayRef<SourceUnit> inputs);

void executeInput(llvm::StringRef input, InputFormat format,
                  const RunOptions &options, llvm::raw_ostream &output);
void executeInputs(llvm::ArrayRef<SourceUnit> inputs, InputFormat format,
                   const RunOptions &options, llvm::raw_ostream &output);
void printJsonError(const std::exception &error, llvm::raw_ostream &output,
                    bool pretty = false);
void printSchema(llvm::raw_ostream &output);

} // namespace lotus::datalog::frontend
