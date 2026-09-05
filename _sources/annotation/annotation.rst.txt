Annotation Framework
===================

Lotus provides annotation frameworks for specifying and analyzing program behavior. The annotation system is organized into several components for tracking argument positions, modification/reference effects, pointer effects, and taint analysis.

Dedicated module pages:

- :doc:`modref` for external mod/ref summaries
- :doc:`pointer_effects` for pointer-side-effect specifications
- :doc:`taint_config` for taint source, sink, and propagation specs

Position Tracking
-----------------

The position tracking system provides abstractions for identifying function arguments and return values in annotations.

**Location**: ``include/Annotation/ArgPosition.h``

**Components**:

* **ArgPosition** – Represents a specific function argument position
* **RetPosition** – Represents the return value position
* **APosition** – Union type that can represent either an argument or return position

**Usage**:
.. code-block:: cpp

   #include <Annotation/ArgPosition.h>
   using namespace annotation;
   
   // Create argument positions
   APosition arg0 = APosition::getArgPosition(0);
   APosition arg1 = APosition::getAfterArgPosition(1);  // After argument 1
   APosition ret = APosition::getReturnPosition();
   
   // Query position type
   if (ret.isReturnPosition()) {
       // Handle return position
   }
   if (arg0.isArgPosition()) {
       const ArgPosition& pos = arg0.getAsArgPosition();
       uint8_t idx = pos.getArgIndex();
       bool after = pos.isAfterArgPosition();
   }

ModRef Annotations
------------------

Modification and reference behavior annotations track how functions access memory through their arguments and return values.

**Location**: ``include/Annotation/ModRef/``

**Components**:

* **ModRefType** – Enumeration: ``Mod`` (modification) or ``Ref`` (reference/read)
* **ModRefClass** – Enumeration: ``DirectMemory`` or ``ReachableMemory``
* **ModRefEffect** – Represents a single mod/ref effect with type, class, and position
* **ModRefEffectSummary** – Collection of mod/ref effects for a function
* **ExternalModRefTable** – Table mapping function names to their mod/ref summaries
* **ExternalModRefTablePrinter** – Utility for printing mod/ref tables

**Usage**:
.. code-block:: cpp

   #include <Annotation/ModRef/ExternalModRefTable.h>
   #include <Annotation/ModRef/ModRefEffect.h>
   using namespace annotation;
   
   // Load mod/ref table from file
   ExternalModRefTable table = ExternalModRefTable::loadFromFile("modref.spec");
   
   // Lookup function effects
   llvm::StringRef funcName = "strcpy";
   const ModRefEffectSummary* summary = table.lookup(funcName);
   
   if (summary) {
       for (const auto& effect : *summary) {
           ModRefType type = effect.getType();
           ModRefClass mClass = effect.getClass();
           APosition pos = effect.getPosition();
           
           if (effect.isModEffect() && effect.onDirectMemory()) {
               // Function modifies direct memory
           }
       }
   }
   
   // Print table
   ExternalModRefTablePrinter printer(llvm::errs());
   printer.printTable(table);

Pointer Annotations
-------------------

Pointer annotations specify how functions allocate, copy, or exit with pointers.

**Location**: ``include/Annotation/Pointer/``

**Components**:

* **PointerEffectType** – Enumeration: ``Alloc``, ``Copy``, or ``Exit``
* **PointerAllocEffect** – Represents pointer allocation with optional size position
* **CopySource** – Source of a copy operation (Value, DirectMemory, ReachableMemory, Null, Universal, Static)
* **CopyDest** – Destination of a copy operation (Value, DirectMemory, ReachableMemory)
* **PointerCopyEffect** – Represents a copy operation from source to destination
* **PointerExitEffect** – Represents program exit
* **PointerEffect** – Union type representing any pointer effect
* **PointerEffectSummary** – Collection of pointer effects for a function
* **ExternalPointerTable** – Table mapping function names to their pointer effect summaries
* **ExternalPointerTablePrinter** – Utility for printing pointer tables

**Usage**:
.. code-block:: cpp

   #include <Annotation/Pointer/ExternalPointerTable.h>
   #include <Annotation/Pointer/PointerEffect.h>
   using namespace annotation;
   
   // Load pointer table from file
   ExternalPointerTable table = ExternalPointerTable::loadFromFile("ptr.spec");
   
   // Lookup function effects
   llvm::StringRef funcName = "malloc";
   const PointerEffectSummary* summary = table.lookup(funcName);
   
   if (summary) {
       for (const auto& effect : *summary) {
           switch (effect.getType()) {
               case PointerEffectType::Alloc: {
                   const PointerAllocEffect& alloc = effect.getAsAllocEffect();
                   if (alloc.hasSizePosition()) {
                       APosition sizePos = alloc.getSizePosition();
                   }
                   break;
               }
               case PointerEffectType::Copy: {
                   const PointerCopyEffect& copy = effect.getAsCopyEffect();
                   const CopyDest& dest = copy.getDest();
                   const CopySource& src = copy.getSource();
                   break;
               }
               case PointerEffectType::Exit:
                   // Function exits program
                   break;
           }
       }
   }
   
   // Print table
   ExternalPointerTablePrinter printer(llvm::errs());
   printer.printTable(table);

Taint Analysis Annotations
--------------------------

Taint analysis annotations specify information flow and taint propagation through functions.

**Location**: ``include/Annotation/Taint/``

**Components**:

* **TaintSpec** – Specification for taint location (ARG, AFTER_ARG, RET), access mode (VALUE, DEREF), and taint type (TAINTED, UNINITIALIZED)
* **PipeSpec** – Specification for taint propagation from source to destination
* **FunctionTaintConfig** – Complete taint configuration for a single function (sources, sinks, pipes)
* **TaintConfig** – Complete taint configuration containing all function specs
* **TaintConfigParser** – Parser for taint specification files (.spec format)
* **TaintConfigManager** – Singleton manager for taint configurations with function name normalization

**Usage**:
.. code-block:: cpp

   #include <Annotation/Taint/TaintConfigManager.h>
   using namespace taint_config;
   
   // Load configuration (singleton pattern)
   TaintConfigManager& manager = TaintConfigManager::getInstance();
   manager.load_config("config/taint.spec");
   // Or use convenience namespace functions
   load_config("config/taint.spec");
   load_default_config();  // Tries multiple standard paths
   
   // Check if function is a source or sink
   bool isSrc = is_source("read");
   bool isSink = is_sink("printf");
   bool isIgnored = is_ignored("memcpy");
   
   // Check LLVM call instructions
   llvm::CallInst* call = ...;
   if (is_source(call)) {
       // Function call is a taint source
   }
   
   // Get detailed function configuration
   const FunctionTaintConfig* funcConfig = get_function_config("strcpy");
   if (funcConfig) {
       // Access source_specs, sink_specs, pipe_specs
   }
   
   // Normalize function names (handles platform prefixes)
   std::string normalized = normalize_name("__strcpy_chk");  // Returns "strcpy"
   
   // Dump configuration
   dump_config(llvm::errs());
   
   // Get statistics
   size_t numSources = get_source_count();
   size_t numSinks = get_sink_count();
