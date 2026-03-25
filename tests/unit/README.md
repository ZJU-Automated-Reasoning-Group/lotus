# Lotus Unit Tests

This directory contains organized unit tests for the Lotus analysis framework. The tests are structured to mirror the component organization of the codebase.

## Directory Structure

```
tests/unit/
├── ControlFlow/       # Control flow graph tests (CFG, ICFG)
├── DataFlow/          # Data flow analysis tests
│   ├── IfdsIde/       # IFDS/IDE solver tests
│   └── Mono/          # Monotone dataflow tests
├── Pointer/           # Pointer and alias analysis tests
├── Utils/             # Utility class and helper tests
└── TestUtils/         # Common test utilities and helpers
```

## Migration Status

### Completed
- ✅ Organized directory structure created
- ✅ TestUtils helper infrastructure (TestConfig.h)
- ✅ CMakeLists.txt files for all subdirectories
- ✅ Integration with main tests/CMakeLists.txt
- ✅ EquivalenceClassMap unit tests migrated from phasar

### Existing Tests (Reorganized)
The following tests were reorganized from the flat structure:

**ControlFlow/**
- ICFGTest.cpp - Interprocedural control flow graph tests

**DataFlow/IfdsIde/**
- IFDSSolverTest.cpp - IFDS solver framework tests
- TaintAnalysisTest.cpp - Taint analysis tests

**DataFlow/Mono/**
- MonoTest.cpp - Monotone dataflow analysis tests

**DataFlow/**
- WPDSTest.cpp - Weighted pushdown system tests

**Pointer/**
- AllocAATest.cpp - Allocation-based alias analysis tests
- AserPTATest.cpp - Aser pointer analysis tests
- DyckAATest.cpp - Dyck-based alias analysis tests
- SparrowAATest.cpp - Sparrow alias analysis tests

**Utils/**
- EGraphTest.cpp - E-graph data structure tests
- EquivalenceClassMapTest.cpp - Equivalence class map ADT tests
- LowerSelectTest.cpp - Select instruction lowering tests
- MHPAnalysisTest.cpp - May-happen-in-parallel analysis tests
- pdg_slicing_test.cpp - Program dependence graph slicing tests
- RemoveDeadBlockTest.cpp - Dead block elimination tests
- SymAbsTest.cpp - Symbolic abstraction tests

## Notes on Phasar Migration

The migration from phasar-development encountered several challenges:

1. **API Differences**: Phasar and Lotus have different APIs for core components like CFG, ICFG, and pointer analysis. Direct migration of many tests would require extensive adaptation.

2. **Infrastructure Dependencies**: Many phasar tests depend on phasar-specific infrastructure:
   - `LLVMProjectIRDB` for IR database management
   - `HelperAnalyses` for analysis orchestration
   - `LLVMBasedCFG/ICFG` classes with specific query interfaces
   - Utility functions like `getNthInstruction()`, `getNthTermInstruction()`

3. **Test Methodology**: Phasar uses a more comprehensive test infrastructure with helper functions for source code location tracking and result comparison that would need to be reimplemented for Lotus.

4. **Recommended Approach**: For future test migration, consider:
   - Creating Lotus-specific test utilities that mirror phasar's helper infrastructure
   - Adapting tests to use Lotus's native APIs rather than direct translation
   - Focusing on algorithm/logic tests rather than API-specific tests
   - Building up a library of common test utilities incrementally

## Running Tests

```bash
# Build and run all tests
mkdir build && cd build
cmake ..
make
make test

# Run only unit tests
make run_unit_tests

# Run specific test
./bin/equivalence_class_map_test
```

## Adding New Tests

Use the helper functions defined in `tests/CMakeLists.txt`:

```cmake
# For regular tests
add_lotus_test(test_name source_file.cpp)

# For PDG-related tests (needs LLVMTransformUtils)
add_lotus_pdg_test(test_name source_file.cpp)
```

Tests are automatically registered with CTest and will be run with `make test`.

## Test Configuration

The `TestUtils/TestConfig.h` file provides common configuration:
- `LOTUS_BUILD_DIR` - Build directory path
- `LOTUS_SRC_DIR` - Source directory path
- `PathToLLTestFiles` - Path to LLVM test files
- `PathToTxtTestFiles` - Path to text test files
- `PathToJSONTestFiles` - Path to JSON test files

