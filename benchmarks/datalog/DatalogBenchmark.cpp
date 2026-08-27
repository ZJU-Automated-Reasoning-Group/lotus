#include "Dataflow/Datalog/Core/Program.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

using namespace lotus::datalog;

namespace {

struct Options {
  std::string workload = "insert";
  std::size_t size = 100000;
  std::size_t workers = 1;
  std::size_t grain_size = 256;
};

struct Result {
  std::uint64_t elapsed_ns = 0;
  std::uint64_t rerun_ns = 0;
  std::size_t result_rows = 0;
  std::size_t checksum = 0;
  std::size_t peak_rss_bytes = 0;
  ExecutionStats stats;
};

std::size_t parseSize(const char *text, const char *option) {
  char *end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (!end || *end != '\0' || value == 0)
    throw std::invalid_argument(std::string(option) +
                                " requires a positive integer");
  return static_cast<std::size_t>(value);
}

Options parseOptions(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--workload" && index + 1 < argc)
      options.workload = argv[++index];
    else if (argument == "--size" && index + 1 < argc)
      options.size = parseSize(argv[++index], "--size");
    else if (argument == "--workers" && index + 1 < argc)
      options.workers = parseSize(argv[++index], "--workers");
    else if (argument == "--grain-size" && index + 1 < argc)
      options.grain_size = parseSize(argv[++index], "--grain-size");
    else
      throw std::invalid_argument("unknown or incomplete option '" + argument +
                                  "'");
  }
  return options;
}

ExecutionOptions executionOptions(const Options &options) {
  ExecutionOptions execution;
  execution.worker_count = options.workers;
  execution.parallel_grain_size = options.grain_size;
  execution.collect_profile = true;
  return execution;
}

template <typename Function> std::uint64_t timeNs(Function &&function) {
  const auto begin = std::chrono::steady_clock::now();
  function();
  const auto end = std::chrono::steady_clock::now();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
          .count());
}

std::size_t peakRssBytes() {
#if defined(__unix__) || defined(__APPLE__)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    return 0;
#if defined(__APPLE__)
  return static_cast<std::size_t>(usage.ru_maxrss);
#else
  return static_cast<std::size_t>(usage.ru_maxrss) * 1024;
#endif
#else
  return 0;
#endif
}

Result runInsert(const Options &options) {
  Result result;
  context ctx;
  auto facts = ctx.relation<std::uint64_t, std::uint64_t>("facts");
  result.elapsed_ns = timeNs([&] {
    for (std::size_t index = 0; index < options.size; ++index)
      facts.insert(index, index * 17U + 3U);
  });
  program p(ctx);
  auto compiled = p.compile();
  compiled.run();
  result.stats = compiled.stats();
  result.peak_rss_bytes = peakRssBytes();
  result.result_rows = options.size;
  for (std::size_t index = 0; index < options.size; ++index)
    result.checksum ^=
        std::hash<std::uint64_t>{}(index + 31U * (index * 17U + 3U));
  return result;
}

Result runDuplicateProofs(const Options &options) {
  Result result;
  context ctx;
  auto source = ctx.relation<std::uint64_t, std::uint64_t>("source");
  auto output = ctx.relation<std::uint64_t>("output");
  auto item = ctx.var<std::uint64_t>("item");
  auto bucket = ctx.var<std::uint64_t>("bucket");
  for (std::size_t index = 0; index < options.size; ++index)
    source.insert(index, index % 16U);
  program p(ctx);
  p.rule(output(bucket), source(item, bucket));
  auto compiled = p.compile();
  result.elapsed_ns = timeNs([&] { compiled.run(executionOptions(options)); });
  result.peak_rss_bytes = peakRssBytes();
  result.result_rows = output.rows().size();
  for (const auto &[value] : output.rows())
    result.checksum ^= std::hash<std::uint64_t>{}(value);
  result.stats = compiled.stats();
  return result;
}

Result runClosure(const Options &options, bool incremental) {
  Result result;
  context ctx;
  auto edge = ctx.relation<std::uint64_t, std::uint64_t>("edge");
  auto path = ctx.relation<std::uint64_t, std::uint64_t>("path");
  auto x = ctx.var<std::uint64_t>("x");
  auto y = ctx.var<std::uint64_t>("y");
  auto z = ctx.var<std::uint64_t>("z");
  const std::size_t split = incremental ? options.size / 2 : options.size;
  for (std::size_t index = 0; index + 1 < split; ++index)
    edge.insert(index, index + 1);
  if (incremental) {
    for (std::size_t index = split; index + 1 < options.size; ++index)
      edge.insert(index, index + 1);
  }
  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  result.elapsed_ns = timeNs([&] { compiled.run(executionOptions(options)); });
  if (incremental && split > 0 && split < options.size) {
    edge.insert(split - 1, split);
    result.rerun_ns = timeNs([&] { compiled.run(executionOptions(options)); });
  }
  result.peak_rss_bytes = peakRssBytes();
  result.result_rows = path.rows().size();
  for (const auto &[left, right] : path.rows())
    result.checksum ^= std::hash<std::uint64_t>{}(left * 131U + right);
  result.stats = compiled.stats();
  return result;
}

Result runSkewJoin(const Options &options) {
  Result result;
  context ctx;
  auto seed = ctx.relation<std::uint64_t>("seed");
  auto points_to = ctx.relation<std::uint64_t, std::uint64_t>("points_to");
  auto property = ctx.relation<std::uint64_t, std::uint64_t>("property");
  auto output = ctx.relation<std::uint64_t>("output");
  auto variable = ctx.var<std::uint64_t>("variable");
  auto object = ctx.var<std::uint64_t>("object");
  auto value = ctx.var<std::uint64_t>("value");
  seed.insert(0);
  const std::size_t hot = std::max<std::size_t>(1, options.size / 10);
  for (std::size_t index = 0; index < options.size; ++index) {
    const std::uint64_t owner = index < hot ? 0 : index;
    points_to.insert(owner, index);
    property.insert(index, index * 7U);
  }
  program p(ctx);
  p.rule(output(value), seed(variable) && property(object, value) &&
                            points_to(variable, object));
  auto compiled = p.compile();
  result.elapsed_ns = timeNs([&] { compiled.run(executionOptions(options)); });
  result.peak_rss_bytes = peakRssBytes();
  result.result_rows = output.rows().size();
  for (const auto &[entry] : output.rows())
    result.checksum ^= std::hash<std::uint64_t>{}(entry);
  result.stats = compiled.stats();
  return result;
}

Result runWorkload(const Options &options) {
  if (options.workload == "insert")
    return runInsert(options);
  if (options.workload == "duplicate-proofs")
    return runDuplicateProofs(options);
  if (options.workload == "closure")
    return runClosure(options, false);
  if (options.workload == "incremental")
    return runClosure(options, true);
  if (options.workload == "skew-join")
    return runSkewJoin(options);
  throw std::invalid_argument("unknown workload '" + options.workload + "'");
}

void printResult(const Options &options, const Result &result) {
  std::cout << "{\n"
            << "  \"workload\": \"" << options.workload << "\",\n"
            << "  \"size\": " << options.size << ",\n"
            << "  \"workers\": " << options.workers << ",\n"
            << "  \"elapsed_ns\": " << result.elapsed_ns << ",\n"
            << "  \"rerun_ns\": " << result.rerun_ns << ",\n"
            << "  \"peak_rss_bytes\": " << result.peak_rss_bytes << ",\n"
            << "  \"result_rows\": " << result.result_rows << ",\n"
            << "  \"checksum\": " << result.checksum << ",\n"
            << "  \"tuples_scanned\": " << result.stats.tuples_scanned << ",\n"
            << "  \"inserted_facts\": " << result.stats.inserted_facts << ",\n"
            << "  \"peak_delta\": " << result.stats.peak_delta << ",\n"
            << "  \"index_memory_bytes\": " << result.stats.index_memory_bytes
            << ",\n"
            << "  \"tuple_memory_bytes\": " << result.stats.tuple_memory_bytes
            << ",\n"
            << "  \"uniqueness_memory_bytes\": "
            << result.stats.uniqueness_memory_bytes << ",\n"
            << "  \"base_memory_bytes\": " << result.stats.base_memory_bytes
            << ",\n"
            << "  \"head_derivations\": " << result.stats.head_derivations
            << ",\n"
            << "  \"local_unique_candidates\": "
            << result.stats.local_unique_candidates << ",\n"
            << "  \"global_unique_candidates\": "
            << result.stats.global_unique_candidates << ",\n"
            << "  \"incremental_sccs\": " << result.stats.incremental_sccs
            << ",\n"
            << "  \"rebuilt_sccs\": " << result.stats.rebuilt_sccs << ",\n"
            << "  \"base_delta_facts\": " << result.stats.base_delta_facts
            << ",\n"
            << "  \"jit_compiled_expressions\": "
            << result.stats.jit_compiled_expressions << ",\n"
            << "  \"jit_expression_evaluations\": "
            << result.stats.jit_expression_evaluations << "\n"
            << "}\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parseOptions(argc, argv);
    printResult(options, runWorkload(options));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}
