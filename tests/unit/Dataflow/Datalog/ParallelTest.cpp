#include "TestSupport.h"

#include <atomic>
#include <sstream>

using namespace lotus::datalog;

namespace {

TEST(DatalogTest, ParallelBspMatchesSerialTransitiveClosure) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");
  for (int value = 0; value < 40; ++value)
    edge.insert(value, value + 1);

  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 2;
  compiled.run(options);

  EXPECT_TRUE(path.contains(0, 40));
  EXPECT_EQ(path.rows().size(), 820U);
  EXPECT_GT(compiled.stats().parallel_tasks, 1U);
}

TEST(DatalogTest, ParallelLatticeMergeCoalescesAcrossWorkers) {
  context ctx;
  auto edge = ctx.relation<int, int, int>("edge");
  auto distance = ctx.lattice<int, int, min_lattice<int>>("distance");
  auto source = ctx.var<int>("source");
  auto middle = ctx.var<int>("middle");
  auto target = ctx.var<int>("target");
  auto weight = ctx.var<int>("weight");
  auto current = ctx.var<min_lattice<int>>("current");
  for (int weight_value = 100; weight_value >= 1; --weight_value)
    edge.insert(1, 2, weight_value);
  distance.insert(1, 1, min_lattice<int>(0));

  program p(ctx);
  p.rule(distance(source, target, current + weight),
         distance(source, middle, current) && edge(middle, target, weight));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 1;
  compiled.run(options);

  EXPECT_TRUE(distance.contains(1, 2, min_lattice<int>(1)));
  EXPECT_EQ(distance.rows().size(), 2U);
  EXPECT_GT(compiled.stats().parallel_merge_tasks, 1U);
}

TEST(DatalogTest, SupportsInjectedScheduler) {
  class CountingScheduler final : public Scheduler {
  public:
    std::size_t workerCount() const override { return 2; }
    void
    parallelFor(std::size_t task_count,
                const std::function<void(std::size_t)> &function) override {
      calls += task_count;
      for (std::size_t task = 0; task < task_count; ++task)
        function(task);
    }
    std::size_t calls = 0;
  } scheduler;

  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");
  edge.insert(1, 2);
  edge.insert(2, 3);
  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.scheduler = &scheduler;
  options.parallel_grain_size = 1;
  compiled.run(options);

  EXPECT_TRUE(path.contains(1, 3));
  EXPECT_GT(scheduler.calls, 0U);
}

TEST(DatalogTest, RejectsZeroWorkerScheduler) {
  class ZeroWorkerScheduler final : public Scheduler {
  public:
    std::size_t workerCount() const override { return 0; }
    void parallelFor(std::size_t,
                     const std::function<void(std::size_t)> &) override {
      ADD_FAILURE() << "zero-worker scheduler must not be invoked";
    }
  } scheduler;

  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  input.insert(1);
  program p(ctx);
  p.rule(output(x), input(x));
  auto compiled = p.compile();

  ExecutionOptions options;
  options.scheduler = &scheduler;
  EXPECT_THROW(compiled.run(options), std::invalid_argument);
  EXPECT_TRUE(output.rows().empty());
}

TEST(DatalogTest, CancellationDiscardsDerivedStateAndAllowsRerun) {
  context ctx;
  auto input = ctx.relation<int>("input");
  auto output = ctx.relation<int>("output");
  auto x = ctx.var<int>("x");
  lotus::CancellationSource cancellation;
  for (int value = 1; value <= 16; ++value)
    input.insert(value);

  program p(ctx);
  p.rule(output(lift(
             [&](int value) {
               cancellation.cancel();
               return value;
             },
             x)),
         input(x));
  auto compiled = p.compile();

  ExecutionOptions options;
  options.cancellation = cancellation.token();
  options.worker_count = 4;
  options.parallel_grain_size = 1;
  EXPECT_EQ(compiled.run(options), RunStatus::Cancelled);
  EXPECT_TRUE(output.rows().empty());

  EXPECT_EQ(compiled.run(), RunStatus::Completed);
  EXPECT_TRUE(output.contains(1));
  EXPECT_TRUE(output.contains(16));
}

TEST(DatalogTest, EmitsSccRuleAndDeltaTrace) {
  context ctx;
  auto edge = ctx.relation<int, int>("edge");
  auto path = ctx.relation<int, int>("path");
  auto x = ctx.var<int>("x");
  auto y = ctx.var<int>("y");
  auto z = ctx.var<int>("z");
  edge.insert(1, 2);
  edge.insert(2, 3);
  program p(ctx);
  p.rule(path(x, y), edge(x, y));
  p.rule(path(x, z), path(x, y) && edge(y, z));
  auto compiled = p.compile();
  std::ostringstream trace;
  ExecutionOptions options;
  options.trace_scc = true;
  options.trace_rule = true;
  options.trace_delta = true;
  options.trace_stream = &trace;
  compiled.run(options);

  EXPECT_NE(trace.str().find("SCC"), std::string::npos);
  EXPECT_NE(trace.str().find("rule"), std::string::npos);
  EXPECT_NE(trace.str().find("iteration"), std::string::npos);
}

TEST(DatalogTest, DeduplicatesSetCandidatesWithinParallelTasks) {
  context ctx;
  auto source = ctx.relation<int, int>("source");
  auto output = ctx.relation<int>("output");
  auto item = ctx.var<int>("item");
  auto bucket = ctx.var<int>("bucket");
  for (int value = 0; value < 1024; ++value)
    source.insert(value, 0);
  program p(ctx);
  p.rule(output(bucket), source(item, bucket));
  auto compiled = p.compile();
  ExecutionOptions options;
  options.worker_count = 4;
  options.parallel_grain_size = 64;
  compiled.run(options);

  EXPECT_EQ(output.rows().size(), 1U);
  EXPECT_EQ(compiled.stats().head_derivations, 1024U);
  EXPECT_LT(compiled.stats().local_unique_candidates,
            compiled.stats().head_derivations);
  EXPECT_EQ(compiled.stats().global_unique_candidates, 1U);
}

} // namespace
