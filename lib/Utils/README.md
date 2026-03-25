# Utilities

Common utilities used throughout the Lotus framework.

## ADT

Abstract data structures:

- `TreeStream.cpp/h` – Tree traversal utilities
- `UnionFind.cpp/.h` – Disjoint set data structure
- `WrappedInterval.cpp/.h` – Wrapped interval arithmetic
- `anatree.h` – AnaTree (analysis tree) structure
- `BDD.h` – Binary Decision Diagrams
- `DisjointSet.h` – Alternative disjoint set implementation
- `egraphs.h` – E-graph for equality reasoning
- `EquivalenceClassMap.h` – Equivalence class mapping
- `GraphSlicer.h` – Graph slicing utilities
- `Hashing.h` – Hashing utilities
- `ImmutableMap.h` – Immutable map
- `ImmutableSet.h` – Immutable set
- `ImmutableTree.h` – Immutable tree structure
- `IntervalsList.h` – Interval list
- `MapIterator.h` – Map iterator utilities
- `MapOfSets.h` – Map of sets
- `OrderedSet.h` – Ordered set
- `PdQsort.h` – Parallel distributed quicksort
- `PriorityWorkList.h` – Priority worklist
- `PushPopCache.h` – Push/pop cache
- `SortedVector.h` – Sorted vector
- `TarjanScc.h` – Tarjan's SCC algorithm
- `TwoLevelWorkList.h` – Two-level worklist
- `VectorMap.h` – Vector-based map
- `VectorSet.h` – Vector-based set

### Iterator

Iterator utilities:

- `DereferenceIterator.h` – Dereference iterator
- `filter_iterator.h` – Filter iterator
- `InfixOutputIterator.h` – Infix output iterator
- `IteratorAdaptor.h` – Iterator adaptor
- `IteratorFacade.h` – Iterator facade
- `IteratorRange.h` – Iterator range
- `IteratorTrait.h` – Iterator traits
- `MapValueIterator.h` – Map value iterator
- `UniquePtrIterator.h` – Unique pointer iterator

## Algorithms

Algorithm utilities:

### PathExpressions

- `PathExpressions.h` – Path expression representation
- `Regex.h` – Regular expression for path analysis
- `PathExpressionComputer.h` – Path expression computation
- `RegexToTgf.h` / `RegexToCompactTgf.h` – Regex to TGF format

## Benchmark

- `Microbench.h` – Microbenchmarking utilities

## Formats

Data format parsing and serialization:

- `cJSON.cpp/.h` – JSON parsing (cJSON library)
- `json11.cpp/.hpp` – JSON utilities (json11 library)
- `SExpr.cpp/.h` – S-expression parsing

### pcomb

Parser combinators:

- `pcomb.h` – Parser combinator framework
- `Combinator/` – Parser combinators (Alt, Lazy, Lexeme, Many, Seq, Token)
- `InputStream/` – Input stream abstraction
- `Parser/` – Parser base classes and implementations

## LLVM

LLVM-specific utilities:

- `Debug.cpp/.h` – Debug output helpers
- `Demangle.cpp/.h` – C++ name demangling
- `FIFOWorkList.h` – FIFO worklist
- `GenericGraph.h` – Generic graph utilities
- `GraphWriter.h` – Graph writing utilities
- `InstructionUtils.cpp/.h` – LLVM instruction utilities
- `LLVMBgl.h` – Boost Graph Library integration
- `Log.h` – Logging utilities
- `RecursiveTimer.cpp/.h` – Nested timing
- `Statistics.cpp/.h` – Statistics collection
- `StringUtils.cpp/.h` – String manipulation
- `SystemHeaders.h` – System header includes

### IO

- `FileUtils.cpp/.h` – File I/O operations
- `ReadFile.h` – File reading
- `ReadIR.cpp/.h` – LLVM IR reading
- `WriteIR.cpp/.h` – LLVM IR writing

## Parallel

Multi-threading support:

- `ThreadSafe.h` – Thread-safe wrappers
- `ThreadPool.cpp/.h` – Thread pool
- `Scheduler/ParallelSchedulerPass.cpp/.h` – Parallel pass scheduling
- `Scheduler/PipelineScheduler.cpp/.h` – Pass pipeline scheduling
- `Scheduler/Task.cpp/.h` – Task representation

## Platform

Platform-specific utilities:

- `ProgressBar.cpp/.h` – Console progress display
- `System.cpp/.h` – System information
- `Timer.cpp/.h` – Timing utilities

## Random

Random number generation:

- `RNG.cpp/.h` – Random number generator (Mersenne Twister)

## Types

Type utilities:

- `Nullable.h` – Nullable type wrapper
- `Offset.h` – Offset representation
- `Optional.h` – Optional type
- `range.h` – Range utilities
- `ScopeExit.h` – Scope exit guard
