Verification Backend Abstraction
=================================

Lotus provides a unified interface to multiple verification backends, allowing you to switch between different tools seamlessly.

Overview
--------

The verification backend abstraction layer provides:

* **Unified interface**: Same API for all verification tools
* **Automatic backend selection**: Choose backend based on property type
* **Result normalization**: Consistent result format across backends
* **Easy integration**: Add new backends without changing client code

Supported Backends
------------------

* **seahorn**: SeaHorn CHC-based verification (supports all properties)
* **sifa**: Sifa symbolic abstraction (reachability)
* **symabs_ai**: SymAbsAI framework (reachability, memsafety, overflow)
* **clam**: CLAM abstract interpretation (memsafety, overflow, reachability)

Property Classes
----------------

* **reachability**: Target location is reachable/unreachable
* **memsafety**: Memory safety (null pointer, use-after-free, etc.)
* **overflow**: Integer overflow detection
* **termination**: Program termination

Usage
-----

Basic usage with ``lotus-verify``:

.. code-block:: bash

   # Auto-select backend
   lotus-verify input.bc --property unreach-call --backend auto --run

   # Specify backend explicitly
   lotus-verify input.bc --property memsafety --backend seahorn --run

   # With timeout
   lotus-verify input.bc --property overflow --backend clam --timeout 60 --run

   # Pass extra arguments to backend
   lotus-verify input.bc --property unreach-call --backend seahorn \
     --backend-arg --horn-cex --run

Result Format
-------------

All backends normalize results to a standard format:

* **true**: Property holds (no error found)
* **false**: Property violated (error found)
* **unknown**: Could not determine
* **error**: Verification tool error
* **timeout**: Verification timed out

Example Output
--------------

.. code-block:: text

   backend: seahorn
   property: unreach-call
   command: seahorn input.bc
   result: true
   message: Property holds (no error found)
   exit-code: 0

Programmatic Usage
------------------

.. code-block:: cpp

   #include "Verification/Backend/Backend.h"
   
   using namespace lotus::verification::backend;
   
   BackendRegistry &reg = BackendRegistry::instance();
   auto backend = reg.create("seahorn");
   
   VerificationTask task;
   task.inputBitcode = "input.bc";
   task.property = PropertyClass::Reachability;
   task.timeoutSeconds = 60;
   
   auto cmd = backend->buildCommand(task);
   // Execute command...
   VerificationResultInfo result = backend->parseResult(output, exitCode);
   
   if (result.isSafe()) {
     // Property holds
   } else if (result.hasError()) {
     // Property violated
   }

Adding New Backends
-------------------

To add a new backend, implement the ``IBackend`` interface:

.. code-block:: cpp

   class MyBackend final : public IBackend {
   public:
     const char *name() const override { return "mybackend"; }
     
     bool supports(PropertyClass property) const override {
       return property == PropertyClass::Reachability;
     }
     
     std::vector<std::string>
     buildCommand(const VerificationTask &task) const override {
       return {"mybackend", task.inputBitcode};
     }
     
     VerificationResultInfo parseResult(const std::string &output,
                                        int exitCode) const override {
       VerificationResultInfo info;
       // Parse output and set info.result, info.message, etc.
       return info;
     }
   };

Then register it in ``BackendRegistry::create()``.
