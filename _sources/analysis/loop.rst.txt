Loop Analysis Framework
=======================

``include/Analysis/Loop/`` and ``lib/Analysis/Loop/`` implement the loop-side
analysis stack used by several optimization and verification workflows.

**Main components**:

- ``FunctionLoopAnalyses`` coordinates per-function loop analyses.
- ``LoopContent`` and ``LoopEnvironment`` summarize loop structure and context.
- ``LoopDependenceGraph`` and ``LoopSCCDAG`` model intra-loop dependencies.
- ``LoopIterationSpaceAnalysis`` reasons about loop bounds and iteration space.
- ``MemoryCloningAnalysis`` and ``SCCDAGAttrs`` support loop transformations.

This subsystem builds on LLVM loop information but adds Lotus-specific program
analysis structures.

Using loop facts
----------------

Start from the LLVM loop structure associated with a function, then use the
coordinating analysis to obtain the Lotus summaries needed by a client.
Iteration-space and dependence information describe what is known about a
loop; transformations must still check their own legality conditions before
rewriting the IR.  Analyses should handle loops without a recoverable bound
conservatively rather than treating the absence of a bound as a small loop.

See also :doc:`cfg` and :doc:`../optimization/index`.
