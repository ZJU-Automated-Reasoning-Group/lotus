Verification Driver Abstraction
================================

Lotus provides a unified driver interface to multiple verification backends, allowing you to switch between different tools seamlessly.

Overview
--------

The verification driver abstraction layer provides:

* **Unified interface**: Same API for all verification tools
* **Automatic backend selection**: Choose backend based on property type
* **Result normalization**: Consistent result format across backends
* **Easy integration**: Add new backends without changing client code

Supported Drivers
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

Lotus builds per-backend verification frontends. The drivers are split across
separate executables:

.. code-block:: bash

   # SymAbsAI
   ./build/bin/lotus-verify-symabs-ai input.bc --property unreach-call

   # Sifa
   ./build/bin/lotus-verify-sifa input.bc --function main

   # CLAM (requires -DLOTUS_ENABLE_CLAM=ON)
   ./build/bin/clam input.bc --crab-check=assert

   # SeaHorn (requires -DLOTUS_ENABLE_SEAHORN=ON)
   ./build/bin/seahorn input.bc

Result Format
-------------

All drivers normalize results to a standard format:

* **true**: Property holds (no error found)
* **false**: Property violated (error found)
* **unknown**: Could not determine
* **error**: Verification tool error
* **timeout**: Verification timed out

Example Output
--------------

.. code-block:: text

   driver: seahorn
   property: unreach-call
   result: true
   message: Property holds (no error found)
   exit-code: 0

Programmatic Usage
------------------

.. code-block:: cpp

   #include "Verification/Driver/Backend.h"
   
   using namespace lotus::verification::driver;
   
   DriverRegistry &reg = DriverRegistry::instance();
   auto driver = reg.create("seahorn");
   
   VerificationTask task;
   task.inputBitcode = "input.bc";
   task.property = PropertyClass::Reachability;
   task.timeoutSeconds = 60;
   
   auto cmd = driver->buildCommand(task);
   // Execute command...
   VerificationResultInfo result = driver->parseResult(output, exitCode);
   
   if (result.isSafe()) {
     // Property holds
   } else if (result.hasError()) {
     // Property violated
   }

Adding New Drivers
-------------------

To add a new driver, implement the ``IDriver`` interface:

.. code-block:: cpp

   class MyDriver final : public IDriver {
   public:
     const char *name() const override { return "mydriver"; }
     
     bool supports(PropertyClass property) const override {
       return property == PropertyClass::Reachability;
     }
     
     std::vector<std::string>
     buildCommand(const VerificationTask &task) const override {
       return {"mydriver", task.inputBitcode};
     }
     
     VerificationResultInfo parseResult(const std::string &output,
                                        int exitCode) const override {
       VerificationResultInfo info;
       // Parse output and set info.result, info.message, etc.
       return info;
     }
   };

Then register it in ``DriverRegistry::create()``.
