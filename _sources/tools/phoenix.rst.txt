Phoenix — Batch Analysis Runner
================================

Phoenix is a Python-based batch analysis runner that automates running
multiple alias analysis tools (Phoenix, SVF, TPA, AserPTA, SparrowAA) over
collections of LLVM bitcode files.

**Location**: ``scripts/phoenix/``

Declarative usage
-----------------

Phoenix supports reproducible YAML experiment specifications.  Install its
small Python dependency, then run an experiment:

.. code-block:: bash

   python -m pip install -r scripts/phoenix/requirements.txt
   python scripts/phoenix/src/main.py run scripts/phoenix/experiments/example.yaml

The run writes ``results.jsonl`` as its source of truth, keeps stdout and
stderr logs in the result directory, and records every repetition separately.
This means timeouts, out-of-memory failures, and crashes remain visible rather
than being omitted from a summary average.  Use ``--output results/my-run`` to
choose the result directory.

YAML specs support ``configs`` for explicit analyzer options and ``matrix``
for a Cartesian product of option values.  They can also declare a
``preprocessing`` list; every command is a string list containing ``{input}``
and ``{output}`` placeholders.  The optional ``execution.executor`` mapping
selects ``local`` (default), ``docker`` (requires ``image`` and optional
mounts), or an interactive ``slurm``/``srun`` backend.

Result operations
-----------------

Raw JSONL data can be summarized or exported without losing individual run
outcomes:

.. code-block:: bash

   python scripts/phoenix/src/main.py report results/my-run
   python scripts/phoenix/src/main.py export results/my-run --format csv
   python scripts/phoenix/src/main.py compare results/baseline results/new

Each run writes ``manifest.json`` with the Lotus commit, platform details,
experiment and benchmark hashes, preprocessing fingerprint, and executable
hashes.  Add optional ``fingerprint_files`` paths (for example, external
library-model specifications) to record their SHA-256 values too.  ``compare``
warns if benchmark or preprocessing fingerprints differ.

Overview
--------

Phoenix provides an interactive command-line interface that:

1. Scans a directory for ``.bc`` files.
2. Lets the user select an analysis tool (Phoenix/SVF/TPA/Aser/Sparrow).
3. Runs the selected tool on each bitcode file.
4. Collects and writes results to structured output files.

Available Analyzers
-------------------

- ``phoenix_analyzer.py`` — Phoenix alias analysis driver
- ``svf_analyzer.py`` — SVF analysis driver
- ``tpa_analyzer.py`` — TPA (Type-based Pointer Analysis) driver
- ``aser_analyzer.py`` — AserPTA analysis driver
- ``sparrow_analyzer.py`` — SparrowAA analysis driver

Usage
-----

.. code-block:: bash

   cd lotus
   python scripts/phoenix/src/main.py

The script will prompt for a directory path containing ``.bc`` files, then ask
which tool to run.  This legacy workflow is retained while users migrate to
YAML experiments.

Operational notes
-----------------

Use a directory containing bitcode built with a compatible LLVM version and
ensure the selected analyzer is available in the expected build location.
Phoenix is a batch runner: it records each input's outcome, but does not make
results from different analyzers directly comparable unless they were run with
the same bitcode, library model, and options.  Keep generated output outside
the input corpus when repeatability matters.
