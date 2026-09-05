Alias Specification Manager
===========================

``include/Alias/Infrastructure/Spec/`` and ``lib/Alias/Infrastructure/Spec/`` provide a unified specification
layer for library functions used by pointer and alias analyses.

**Main components**:

- ``AliasSpecManager`` loads and serves per-function specifications.
- ``FunctionCategory`` classifies library routines.
- ``AllocatorInfo``, ``CopyInfo``, ``ReturnAliasInfo``, and ``ModRefInfo`` model
  specialized behaviors that analyses need.

This layer complements the generic annotation subsystem by exposing a
pointer-analysis-oriented API.

What specifications express
---------------------------

Specifications give an analysis information that may be absent from a
declaration-only library call.  For example, an allocator specification can
identify the size argument, whether the allocation is zero-initialized, and
whether the result is returned directly or written through an out parameter.
Copy specifications record source and destination arguments, while
return-alias specifications describe routines such as ``strcpy`` whose return
value aliases an argument.

The manager also recognizes deallocators, no-effect functions, exit routines,
and mod/ref effects.  A function can have more than one applicable category;
clients that need the complete classification should use ``getCategories``
rather than the single primary category returned by ``getCategory``.

Using the manager
-----------------

Construct an ``AliasSpecManager`` once for an analysis pass, optionally bind
it to the module with ``initialize``, and query it while modeling calls.  Both
``llvm::Function`` and function-name overloads are available, which is useful
when an analysis first builds name-based seed sets.  The default constructor
loads the standard specification files; callers may provide their own files or
load additional ones with ``loadSpecFile``.  After changing specifications at
runtime, clear the query cache before relying on newly added entries.

Specifications refine models; they do not replace the ordinary handling of
defined functions.  Clients should continue to analyze available function
bodies and use the manager to give known declarations their library semantics.

See also :doc:`../annotation/modref`, :doc:`../annotation/pointer_effects`, and
:doc:`alias_analysis`.
