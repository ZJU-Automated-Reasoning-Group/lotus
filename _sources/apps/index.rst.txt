Applications
============

This section covers the application-oriented parts of Lotus that currently have
dedicated documentation here.

Components
----------

* **Fuzzing**: Directed greybox fuzzing support and analyses
  (``lib/Fuzzing/``)

At the moment this section mainly tracks the directed fuzzing stack under
``lib/Fuzzing/``. The pages below distinguish between the analysis layer and the
compiler or linker side pieces that are still present in the source tree.


.. toctree::
   :maxdepth: 2

   fuzzing_support
   fuzzing_analysis
   aflgo_compiler
   aflgo_linker
