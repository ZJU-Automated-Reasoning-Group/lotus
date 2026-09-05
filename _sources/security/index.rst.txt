Security Components
===================

This section covers Lotus components for side-channel analysis, speculative
execution analysis, and mitigation-oriented program transformation.

Overview
--------

At a glance:

- **ConstantTime** (``lib/Security/ConstantTime``): CT-LLVM-style
  constant-time analysis for secret-dependent side channels. See
  :doc:`crypto`.
- **LIF** (``lib/Security/LIF``): Taint-guided isochronous transformation and
  control-flow linearization for side-channel mitigation. See :doc:`lif`.
- **Spectre** (``lib/Security/Spectre``): Spectre-v1-style speculative cache
  analysis for secret-dependent cache behavior. See :doc:`spectre`.

These components complement the reusable analysis utilities documented under
:doc:`../analysis/index` and the checker-oriented front-ends documented under
:doc:`../checker/index`.

.. toctree::
   :maxdepth: 2

   crypto
   lif
   spectre
