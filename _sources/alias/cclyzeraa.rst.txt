CclyzerAA
=========

``CclyzerAA`` is an alias-analysis integration layer for the cclyzer-style
points-to pipeline.

**Headers**: ``include/Alias/InclusionBased/CclyzerAA/``

**Implementation**: ``lib/Alias/InclusionBased/CclyzerAA/``

Overview
--------

This component exposes a small API for constructing and querying cclyzer-based
alias information over an LLVM module. In the current tree it is primarily a
library component rather than a standalone front-end binary.

Primary entry point
-------------------

.. code-block:: cpp

   #include "Alias/InclusionBased/CclyzerAA/CclyzerAA.h"

   lotus::cclyzer::CclyzerAA analysis;
   analysis.analyze(module);

Typical use cases
-----------------

- Compare cclyzer-style results with the in-tree Lotus pointer analyses.
- Reuse an external points-to engine through a lightweight wrapper API.
- Prototype experiments without wiring a new command-line tool first.

See also
--------

- See :doc:`alias_analysis` for the full alias-analysis landscape in Lotus.
- See :doc:`aserpta`, :doc:`sparrowaa`, and :doc:`lotusaa` for in-tree
  alternatives with dedicated front-ends.
