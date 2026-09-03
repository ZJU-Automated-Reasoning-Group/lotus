Alias Analysis
==============

This section covers the various alias analysis algorithms implemented in Lotus.

Start with :doc:`alias_analysis` for a high-level selection guide.  Algorithm
pages describe the assumptions and entry points of individual analyses, while
the infrastructure pages cover common points-to-set representations, external
library specifications, and evaluation metrics.  Results depend on the LLVM
module and the model of external calls, so use the specification and metrics
pages alongside an algorithm page when comparing analyses.

.. toctree::
   :maxdepth: 2

   alias_analysis
   allocaa
   sparrowaa
   aserpta
   cclyzeraa
   dda
   dfpa
   dyckaa
   seadsa
   fpa
   flowsensitive
   lotusaa
   underapproxaa
   dynaa
   sraa
   metrics
   ptsset
   spec
   tpa
   typequalifier
