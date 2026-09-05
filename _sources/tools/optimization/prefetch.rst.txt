Prefetch Optimization Tool
==========================

This page documents the software prefetching frontend backed by
``lib/Optimization/Prefetch/``.

**Implementation Location**: ``lib/Optimization/Prefetch/``

Tool
----

- **Binary**: ``lotus-opt-prefetch``
- **Source**: ``tools/optimization/lotus-opt-prefetch.cpp``
- **Pass**: ``SWPrefetchingLLVMPass``

Example:

.. code-block:: bash

   build/bin/lotus-opt-prefetch input.bc \
     --prefetch-distance-provider=profile \
     --profile=perf.prof \
     -o prefetched.bc

   build/bin/lotus-opt-prefetch input.bc \
     --prefetch-distance-provider=lbr \
     --dist=32 \
     -o prefetched-lbr.bc

Configuration
-------------

- ``--profile=<file>`` supplies the sample-profile input used by the profile
  provider.
- ``--prefetch-distance-provider=profile`` uses sample-profile hints.
- ``--prefetch-distance-provider=lbr`` uses explicit LBR distances from
  ``--dist``.
- ``--prefetch-distance-provider=llm`` uses explicit distances from
  ``--llm-dist``.
- ``--prefetch-distance-provider=static`` reserves the static-analysis mode.
- ``--input-file`` remains accepted as the underlying legacy pass option, but
  ``--profile`` is the preferred frontend spelling.

Notes
-----

- The pass is registered from
  ``lib/Optimization/Prefetch/SWPrefetchingPass.cpp``.
- The implementation uses loop analysis and inserts LLVM prefetch intrinsics for
  supported access patterns.

Choosing a provider
-------------------

Use the profile provider when a representative sample profile is available;
use LBR or LLM providers only when their supplied distance data matches the
compiled program.  Prefetching is a performance optimization, not a semantic
transformation, and its benefit depends on access patterns and target hardware.
Benchmark the emitted program against the original one and inspect the output
IR when tuning distances or enabling a new provider.
