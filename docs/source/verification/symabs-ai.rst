SymAbsAI – Symbolic Abstraction + Abstract Interpretation
==========================================================

A framework for static program analysis using symbolic abstraction on LLVM IR.

**Headers**: ``include/Verification/SymAbsAI``

**Implementation**: ``lib/Verification/SymAbsAI``

**Main components**:

- **Analyzer** – Fixpoint engine that drives abstract interpretation over a function
- **FragmentDecomposition** – Partitions CFG into acyclic fragments for scalable analysis
- **DomainConstructor** – Factory for creating and composing abstract domains
- **FunctionContext** – Per-function analysis context and state management
- **ModuleContext** – Module-level context for interprocedural setup
- **SymAbsAIPass** – LLVM function pass that integrates SymAbsAI into optimization pipelines
- **AbstractValue** – Base interface for abstract domain values
- **InstructionSemantics** – Converts LLVM instructions to SMT expressions

**Abstract Domains** (in ``domains/``):

- **NumRels** – Numerical relations (e.g., ``x <= y + 5``)
- **Intervals** – Value range analysis (e.g., ``x ∈ [0, 100]``)
- **Affine** – Affine relationships (e.g., ``y = 2*x + 3``)
- **BitMask** – Bit-level tracking and alignment
- **SimpleConstProp** – Constant propagation
- **Boolean** – Boolean truth values and invariants
- **Predicates** – Path predicates and assertions
- **MemRange** – Memory access bounds in terms of function arguments
- **MemRegions** – Memory region and pointer analysis

**Typical use cases**:

- Constant propagation and dead code elimination
- Bounds checking and array access verification
- Bit-level analysis and alignment tracking
- Numerical invariant discovery
- Memory safety analysis
- Custom abstract interpretation passes

**Basic usage (C\+\+)**:

.. code-block:: cpp

   #include <Verification/SymAbsAI/Core/SymAbsAIPass.h>
   #include <Verification/SymAbsAI/Analyzers/Analyzer.h>
   #include <Verification/SymAbsAI/Core/FragmentDecomposition.h>
   #include <Verification/SymAbsAI/Core/DomainConstructor.h>

   // Using SymAbsAIPass as an LLVM pass
   llvm::Function &F = ...;
   symabs_ai::SymAbsAIPass pass;
   pass.runOnFunction(F);

   // Or using the Analyzer directly
   auto mctx = std::make_unique<symabs_ai::ModuleContext>(F.getParent(), config);
   auto fctx = mctx->createFunctionContext(&F);
   auto fragments = symabs_ai::FragmentDecomposition::For(*fctx, 
       symabs_ai::FragmentDecomposition::Headers);
   symabs_ai::DomainConstructor domain = /* construct domain */;
   auto analyzer = symabs_ai::Analyzer::New(*fctx, fragments, domain);
   analyzer->run();

   // Query results
   llvm::BasicBlock *BB = ...;
   const symabs_ai::AbstractValue *state = analyzer->at(BB);

**Fragment Strategies**:

- **Edges** – Abstract after every basic block (most precise, slowest)
- **Function** – Analyze whole function as one fragment (fastest, least precise)
- **Headers** – Place abstraction points at loop headers (good balance)
- **Body** – Abstract in loop bodies
- **Backedges** – Abstract at loop backedges

**Integration**:

SymAbsAI can be used as:

- An LLVM ``FunctionPass`` via ``SymAbsAIPass``
- A standalone analysis library via ``Analyzer``
- A tool via ``build/bin/symabs_ai`` (see :doc:`../tools/verifier/symabs-ai/index`)

For more details on using SymAbsAI as a tool, see :doc:`../tools/verifier/symabs-ai/index`.


