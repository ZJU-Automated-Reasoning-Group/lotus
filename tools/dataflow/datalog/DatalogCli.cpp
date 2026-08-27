#include "Dataflow/Datalog/Frontend/Frontend.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

namespace {

void usage(llvm::raw_ostream &output) {
  output << "usage:\n"
            "  lotus-datalog run <source...|-> [options]\n"
            "  lotus-datalog explain <source...|-> [--analyze] [options]\n"
            "  lotus-datalog validate <source...|-> [options]\n"
            "  lotus-datalog schema\n\n"
            "options:\n"
            "  --format auto|json|datalog|z3\n"
            "  --workers N\n"
            "  --grain-size N\n"
            "  --pretty\n"
            "  --trace-scc\n"
            "  --trace-rule\n"
            "  --trace-delta\n"
            "  --analyze\n";
}

std::size_t parseSize(const char *value, const char *option) {
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (!end || *end != '\0' || parsed == 0)
    throw std::invalid_argument(std::string(option) +
                                " requires a positive integer");
  return static_cast<std::size_t>(parsed);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(llvm::errs());
    return 2;
  }

  const std::string command = argv[1];
  if (command == "schema") {
    lotus::datalog::frontend::printSchema(llvm::outs());
    return 0;
  }
  if (command != "run" && command != "validate" && command != "explain") {
    usage(llvm::errs());
    return 2;
  }
  lotus::datalog::frontend::RunOptions options;
  lotus::datalog::frontend::InputFormat format =
      lotus::datalog::frontend::InputFormat::Auto;
  options.validate_only = command == "validate";
  options.explain = command == "explain";
  options.execution.trace_stream = &std::cerr;
  options.source_resolver =
      [](llvm::StringRef including_source, llvm::StringRef requested_path)
      -> std::optional<lotus::datalog::frontend::OwnedSourceUnit> {
    llvm::SmallString<256> resolved(requested_path);
    if (!llvm::sys::path::is_absolute(resolved)) {
      llvm::SmallString<256> directory(including_source);
      llvm::sys::path::remove_filename(directory);
      llvm::sys::path::append(directory, requested_path);
      resolved = directory;
    }
    llvm::sys::path::remove_dots(resolved, true);
    auto buffer = llvm::MemoryBuffer::getFile(resolved);
    if (!buffer)
      return std::nullopt;
    return lotus::datalog::frontend::OwnedSourceUnit{
        resolved.str().str(), (*buffer)->getBuffer().str()};
  };
  std::vector<std::string> paths;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--format" && index + 1 < argc) {
      try {
        format = lotus::datalog::frontend::parseInputFormat(argv[++index]);
      } catch (const std::invalid_argument &error) {
        llvm::errs() << error.what() << '\n';
        return 2;
      }
    } else if (argument == "--workers" && index + 1 < argc) {
      options.execution.worker_count = parseSize(argv[++index], "--workers");
    } else if (argument == "--grain-size" && index + 1 < argc) {
      options.execution.parallel_grain_size =
          parseSize(argv[++index], "--grain-size");
    } else if (argument == "--pretty") {
      options.pretty = true;
    } else if (argument == "--trace-scc") {
      options.execution.trace_scc = true;
    } else if (argument == "--trace-rule") {
      options.execution.trace_rule = true;
    } else if (argument == "--trace-delta") {
      options.execution.trace_delta = true;
    } else if (argument == "--analyze" && options.explain) {
      options.explain_analyze = true;
    } else if (!argument.empty() && argument.front() != '-') {
      paths.push_back(argument);
    } else if (argument == "-") {
      paths.push_back(argument);
    } else {
      llvm::errs() << "unknown option: " << argument << '\n';
      return 2;
    }
  }

  if (paths.empty()) {
    llvm::errs() << "missing input source path\n";
    return 2;
  }
  std::size_t stdin_count = 0;
  for (const std::string &path : paths)
    stdin_count += path == "-";
  if (stdin_count > 1) {
    llvm::errs() << "standard input may be specified only once\n";
    return 2;
  }

  std::vector<std::unique_ptr<llvm::MemoryBuffer>> buffers;
  buffers.reserve(paths.size());
  for (const std::string &path : paths) {
    auto input = llvm::MemoryBuffer::getFileOrSTDIN(path);
    if (!input) {
      llvm::errs() << "cannot read '" << path
                   << "': " << input.getError().message() << '\n';
      return 2;
    }
    buffers.push_back(std::move(*input));
  }

  std::vector<lotus::datalog::frontend::SourceUnit> sources;
  sources.reserve(paths.size());
  for (std::size_t index = 0; index < paths.size(); ++index) {
    sources.push_back({paths[index], buffers[index]->getBuffer()});
  }

  try {
    lotus::datalog::frontend::executeInputs(sources, format, options,
                                            llvm::outs());
  } catch (const std::exception &error) {
    lotus::datalog::frontend::printJsonError(error, llvm::outs(),
                                             options.pretty);
    return 1;
  }
  return 0;
}
