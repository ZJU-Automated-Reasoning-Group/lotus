Instrumentation Passes
=======================

Lotus provides several LLVM instrumentation passes for preparing programs for verification.

Available Passes
----------------

BreakCritLoops
~~~~~~~~~~~~~~

Breaks critical loops for better slicing and control dependence analysis.

**Usage:**

.. code-block:: bash

   lotus-opt input.bc -break-crit-loops -o output.bc

**What it does:**

* Identifies basic blocks that jump back to themselves via critical edges
* Splits these blocks to improve control dependence computation
* Useful before running PDG-based slicing

DeleteUndefined
~~~~~~~~~~~~~~~

Deletes calls to undefined functions and replaces non-void undefined functions with nondeterministic values.

**Usage:**

.. code-block:: bash

   lotus-opt input.bc -delete-undefined -o output.bc

**What it does:**

* Removes calls to undefined void functions
* Replaces undefined non-void functions with ``verifier.nondet.undef.*`` calls
* Preserves calls to verifier functions and standard library functions
* Useful for preparing programs with missing function definitions

InitializeUninitialized
~~~~~~~~~~~~~~~~~~~~~~~

Initializes stack-allocated variables with nondeterministic values.

**Usage:**

.. code-block:: bash

   lotus-opt input.bc -init-uninit -o output.bc

**What it does:**

* Finds all ``alloca`` instructions in function entry blocks
* Replaces uninitialized uses with calls to ``verifier.nondet.init.*`` functions
* Useful for verification where uninitialized variables should be treated as nondeterministic

MakeNondet
~~~~~~~~~~

Replaces selected input functions with nondeterministic value generators.

**Usage:**

.. code-block:: bash

   lotus-opt input.bc -make-nondet -o output.bc

**Configuration:**

The pass accepts a comma-separated list of function names to replace:

.. code-block:: bash

   lotus-opt input.bc -make-nondet \
     --make-nondet-targets=rand,getchar,fgetc,scanf \
     -o output.bc

**What it does:**

* Replaces calls to specified functions (e.g., ``rand()``, ``getchar()``) with nondeterministic values
* Preserves return type semantics
* Useful for abstracting away I/O and random number generation

PrepareOverflows
~~~~~~~~~~~~~~~~

Instruments signed arithmetic operations with explicit overflow checks.

**Usage:**

.. code-block:: bash

   lotus-opt input.bc -prep-overflows -o output.bc

**What it does:**

* Finds signed integer arithmetic operations (add, sub, mul)
* Replaces them with overflow-checking intrinsics (``llvm.sadd.with.overflow``, etc.)
* Calls ``__VERIFIER_error()`` if overflow detected
* Useful for overflow property checking

Example Workflow
----------------

A typical verification workflow:

.. code-block:: bash

   # 1. Initialize uninitialized variables
   lotus-opt input.bc -init-uninit -o step1.bc

   # 2. Replace I/O with nondet
   lotus-opt step1.bc -make-nondet -o step2.bc

   # 3. Instrument overflow checks
   lotus-opt step2.bc -prep-overflows -o prepared.bc

   # 4. Verify
   lotus-verify prepared.bc --property overflow --backend clam --run

All Passes Together
-------------------

You can enable all instrumentation passes at once:

.. code-block:: bash

   lotus-opt input.bc -init-uninit -make-nondet -prep-overflows -o output.bc

Or use the convenience flag (if available):

.. code-block:: bash

   lotus-opt input.bc -ip-all -o output.bc

Integration with Verification
-----------------------------

These passes are designed to work with Lotus's verification backends:

* **InitializeUninitialized**: Prepares programs for abstract interpretation (CLAM)
* **MakeNondet**: Essential for symbolic execution (SeaHorn, Sifa)
* **PrepareOverflows**: Required for overflow property checking

See :doc:`verification_backends` for more information on using these with verification tools.
