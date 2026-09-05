Verification Backend API
========================

``include/Verification/Driver/Backend.h`` and ``lib/Verification/Driver/Backend.cpp``
define the shared abstraction used to invoke different verification engines
through one API.

**Main components**:

- ``PropertyClass`` and ``VerificationTask`` describe the requested job.
- ``VerificationResultInfo`` stores standardized results.
- ``IBackend`` is the backend interface.
- ``BackendRegistry`` manages available implementations.

The built-in registry covers SeaHorn, Sifa, SymAbsAI, and Clam.

Backend lifecycle
-----------------

A frontend describes the property and input as a ``VerificationTask``, selects
an implementation from ``BackendRegistry``, and receives a normalized
``VerificationResultInfo``.  This separation lets callers present one result
format even when engines have different command lines or witness formats.
Backend implementations should report unsupported tasks explicitly rather than
silently weakening the requested property.

See also :doc:`index` and :doc:`../user_guide/verification_backends`.
