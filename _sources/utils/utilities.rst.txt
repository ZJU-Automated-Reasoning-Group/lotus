Utility Libraries
==================

Lotus provides comprehensive utility libraries organized into categories: Platform, Random, Types, Benchmark, ADT, Formats, Algorithms, Vendor, LLVM, and Parallel.

Dedicated module pages:

- :doc:`adt`
- :doc:`algorithms`
- :doc:`benchmark`
- :doc:`formats`
- :doc:`llvm`
- :doc:`parallel`
- :doc:`platform`
- :doc:`random`
- :doc:`types`

Platform Utilities
------------------

Platform, timing, and display utilities in ``lib/Utils/Platform/`` and ``include/Utils/Platform/``:

* **System.h** - System information (OS detection, endianness, time utilities)
* **Timer.h** - Timeout and timing utilities
* **ProgressBar.h** - Progress bar display utilities

Random
------

* **RNG.h** (``include/Utils/Random/``) - Random number generator (Mersenne Twister)

Types
~~~~~

Type utilities in ``include/Utils/Types/``:

* **Offset.h** - Offset manipulation utilities
* **Nullable.h** - Nullable type utilities
* **ScopeExit.h** - RAII-style scope exit handlers
* **range.h** - Range utilities

Benchmark
~~~~~~~~~

* **Microbench.h** (``include/Utils/Benchmark/``) - Microbenchmarking utilities

Abstract Data Types (ADT)
~~~~~~~~~~~~~~~~~~~~~~~~~

Data structures in ``lib/Utils/ADT/`` and ``include/Utils/ADT/``:

* **BDD.h** - Binary Decision Diagram implementation
* **DisjointSet.h** - Disjoint set data structure (Union-Find)
* **Hashing.h** - Hash utilities and helpers
* **ImmutableMap.h** / **ImmutableSet.h** / **ImmutableTree.h** - Immutable collections
* **IntervalsList.h** - Interval list data structure
* **MapOfSets.h** - Map of sets container
* **OrderedSet.h** - Ordered set container
* **PriorityWorkList.h** - Priority queue work list
* **PushPopCache.h** - Cache with push/pop semantics
* **SortedVector.h** - Sorted vector implementation
* **TreeStream.h** - Tree-structured stream output
* **TwoLevelWorkList.h** - Two-level work list
* **UnionFind.h** - Union-Find data structure (unsigned-based)
* **VectorMap.h** / **VectorSet.h** - Vector-based map/set containers
* **anatree.h** - Tree analysis utilities
* **GraphSlicer.h** - Graph slicing utilities
* **PdQsort.h** - Parallel quicksort implementation

Iterator utilities (``include/Utils/ADT/Iterator/``):

* **DereferenceIterator.h** - Iterator that dereferences values
* **filter_iterator.h** - Filtered iterator
* **InfixOutputIterator.h** - Output iterator with infix separators
* **IteratorAdaptor.h** / **IteratorFacade.h** - Iterator base classes
* **IteratorRange.h** - Range wrapper for iterators
* **IteratorTrait.h** - Iterator trait utilities
* **MapValueIterator.h** - Iterator over map values
* **UniquePtrIterator.h** - Iterator for unique pointers

**Usage**:
.. code-block:: cpp

   #include "Utils/ADT/DisjointSet.h"
   DisjointSet<int> ds;
   ds.makeSet(0);
   ds.makeSet(1);
   ds.doUnion(0, 1);
   int root = ds.findSet(0);

**Note**: There are two Union-Find implementations in ``include/Utils/ADT/``:
- **UnionFind.h** - Simple unsigned-based implementation with ``mk()``, ``find()``, ``merge()``
- **DisjointSet.h** - Template-based implementation with ``makeSet()``, ``findSet()``, ``doUnion()``

Formats
~~~~~~~

Data format parsing and serialization in ``lib/Utils/Formats/`` and ``include/Utils/Formats/``:

* **cJSON.h** - Lightweight JSON parser/generator (C library)
* **json11.hpp** - C++ JSON library
* **SExpr.h** - S-expression parsing
* **pcomb/** - Parser combinator library

Algorithms
~~~~~~~~~~

Algorithm implementations in ``include/Utils/Algorithms/``:

* **PathExpressions/** - Tarjan's path expression algorithm over labeled graphs (regular expressions describing all paths between two nodes)

Parser Combinator Framework (pcomb):
- **pcomb.h** - Main header
- **Parser/** - Base parser classes (Parser, ParseResult, StringParser, RegexParser, etc.)
- **Combinator/** - Combinators (AltParser, SeqParser, ManyParser, LazyParser, etc.)
- **InputStream/** - Input stream abstractions

Vendor
~~~~~~

Third-party vendored libraries in ``include/Utils/Vendor/``:

* **spdlog/** - Fast C++ logging library
* **CLI11.h** - Command-line argument parsing

Usage
~~~~~

.. code-block:: cpp

   #include "Utils/Platform/Timer.h"
   Timer timer(5.0, []() { /* timeout action */ });
   timer.check(); // Check for timeout

   #include "Utils/Platform/ProgressBar.h"
   ProgressBar bar("Processing", ProgressBar::PBS_CharacterStyle);
   bar.showProgress(0.5); // 50% complete

   #include "Annotation/APISpec.h"
   lotus::APISpec spec;
   spec.loadFile("config/ptr.spec", errorMsg);
   bool ignored = spec.isIgnored("malloc");

Parallel Utilities
------------------

Multi-threading utilities in ``lib/Utils/Parallel/`` and ``include/Utils/Parallel/``:

* **ThreadPool.h** - Thread pool and task group utilities
* **ThreadSafe.h** - Thread-safe container wrappers
* **Cancellation.h** - Cooperative cancellation tokens for parallel work

Recommended ThreadPool patterns
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``ThreadPool`` is the preferred entry point for analysis parallelism.

* Use ``parallelFor()`` for index-based parallel loops.
* Use ``parallelForEach()`` for container traversal when order does not matter.
* Use ``parallelReduce()`` for parallel aggregation followed by deterministic merge on the caller thread.
* Use ``makeThreadLocal()`` or ``makeThreadLocalReducer()`` for per-worker scratch state instead of raw ``void*`` thread locals.
* Use ``CancellationSource`` / ``CancellationToken`` for cooperative cancellation.
* ``TaskGroup::wait()`` is safe to call from worker tasks; workers help drain the pool instead of purely blocking.

**Usage**:
.. code-block:: cpp

   #include "Utils/Parallel/ThreadPool.h"
   ThreadPool* pool = ThreadPool::get();

   pool->parallelFor<int>(0, work_items, 8, [&](int i) {
     analyze(i);
   });

   auto total = pool->parallelReduce<std::size_t>(
       0, values.size(), 16, 0ULL,
       [&](std::size_t i) { return values[i]; },
       [](unsigned long long acc, unsigned long long value) {
         return acc + value;
       });

   auto reducer = pool->makeThreadLocalReducer<std::set<int>>(
       [](std::set<int> acc, const std::set<int>& local) {
         acc.insert(local.begin(), local.end());
         return acc;
       });

   pool->parallelFor<int>(0, 32, 4, [&](int i) {
     reducer.local().insert(i);
   });

   auto all_ids = reducer.reduce({});

Cancellation
~~~~~~~~~~~~

Parallel work uses cooperative cancellation rather than thread interruption.
Pass a token to ``TaskGroup::async()``, ``parallelFor()``, or ``parallelForEach()``
when the work should stop early once a scheduler or caller decides to cancel.

.. code-block:: cpp

   #include "Utils/Parallel/Cancellation.h"
   #include "Utils/Parallel/ThreadPool.h"

   lotus::CancellationSource cancel;
   ThreadPool* pool = ThreadPool::get();
   pool->parallelFor<int>(0, tasks, 4, cancel.token(), [&](int i) {
     if (cancel.token().isCancelled())
       return;
     process(i);
   });

LLVM Utilities
--------------

LLVM-specific utilities located in ``lib/Utils/LLVM/`` and ``include/Utils/LLVM/``.

IR Manipulation and Analysis
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* **InstructionUtils.h** - LLVM instruction utilities (line numbers, names, JSON escaping)
* **Demangle.h** - Name demangling utilities

I/O Utilities
~~~~~~~~~~~~~

* **FileUtils.h** - File system operations (create directory, read/write files)
* **IO/FileUtils.h** - Additional file utilities
* **IO/ReadFile.h** - File reading utilities
* **IO/ReadIR.h** - LLVM IR reading utilities
* **IO/WriteIR.h** - LLVM IR writing utilities
* **StringUtils.h** - String formatting and manipulation

**Usage**:
.. code-block:: cpp

   #include "Utils/LLVM/IO/FileUtils.h"
   FileUtil::createDirectory("output");
   FileUtil::writeToFile("output/result.txt", content);

Debugging and Profiling
~~~~~~~~~~~~~~~~~~~~~~~

* **Debug.h** - Debug output utilities
* **Log.h** - Logging infrastructure (uses spdlog from Utils/Vendor/)
* **Statistics.h** - Analysis statistics collection
* **RecursiveTimer.h** - Recursive timing utilities

**Usage**:
.. code-block:: cpp

   #include "Utils/LLVM/Log.h"
   LOG_INFO("Analysis completed");

   #include "Utils/LLVM/Statistics.h"
   Statistics::counter("functions_analyzed")++;

Work Lists
~~~~~~~~~~

Work list implementations for analysis algorithms:

* **FIFOWorkList.h** (``include/Utils/LLVM/``) - First-in-first-out work list
* **PriorityWorkList.h** / **TwoLevelWorkList.h** (``include/Utils/ADT/``) - See ADT section above

Graph Utilities
~~~~~~~~~~~~~~~

* **GenericGraph.h** - Generic graph data structure
* **GraphWriter.h** - Graph visualization utilities
* **LLVMBgl.h** - LLVM CFG iteration adapters

Scheduler
~~~~~~~~~

Parallel execution scheduling:

* **Scheduler/ParallelSchedulerPass.h** - Parallel scheduler pass
* **Scheduler/PipelineScheduler.h** - Pipeline scheduler
* **Task.h** - Task representation

Multi-threading
~~~~~~~~~~~~~~~

* **ThreadPool.h** - Thread pool implementation for parallel execution

**Usage**:
.. code-block:: cpp

   #include "Utils/Parallel/ThreadPool.h"
   ThreadPool* pool = ThreadPool::get();
   auto future = pool->enqueue([]() { return compute(); });
   auto result = future.get();

System Headers
~~~~~~~~~~~~~~

* **SystemHeaders.h** - Consolidated LLVM and system header includes

Common Usage Patterns
---------------------

**Timer with Timeout**:
.. code-block:: cpp

   #include "Utils/Platform/Timer.h"
   Timer timer(5.0, []() { std::cout << "Timeout!"; });
   while (condition) {
     timer.check(); // Check timeout
     // ... work ...
   }

**Progress Reporting**:
.. code-block:: cpp

   #include "Utils/Platform/ProgressBar.h"
   ProgressBar bar("Analyzing", ProgressBar::PBS_CharacterStyle, 0.01);
   for (size_t i = 0; i < total; ++i) {
     bar.showProgress(float(i) / total);
     // ... work ...
   }

**Thread Pool**:
.. code-block:: cpp

   #include "Utils/Parallel/ThreadPool.h"
   ThreadPool* pool = ThreadPool::get();
   std::vector<std::future<Result>> futures;
   for (auto& item : items) {
     futures.push_back(pool->enqueue([&item]() { return process(item); }));
   }
   for (auto& f : futures) {
     results.push_back(f.get());
   }

**String Formatting**:
.. code-block:: cpp

   #include "Utils/LLVM/StringUtils.h"
   std::string msg = format_str("Function %s has %d instructions",
                                funcName.c_str(), count);

**Logging**:
.. code-block:: cpp

   #include "Utils/LLVM/Log.h"
   LOG_INFO("Starting analysis");
   LOG_DEBUG("Processing function: " << funcName);
   LOG_ERROR("Error occurred: " << errorMsg);
