# LIBSMT

MT solver wrapper library providing a unified interfaceS for SMT solvers.

## Overview

LIBSMT provides a factory-based API for creating and managing SMT solver instances. It wraps the Z3 solver and provides a consistent interface across different solver backends.

## Components

- **SMTFactory** – Factory for creating solver instances
- **SMTExpr** – Expression representation
- **SMTModel** – Model extraction
- **SATSolver** – SAT solver interface
- **SMTLIB2Solver** – SMT-LIB 2 format support
- **Z3Expr** – Z3-specific expression handling
- **CNF** – CNF conversion

## Usage

```cpp
#include "Solvers/SMT/LIBSMT/SMTFactory.h"

auto solver = lotus::smt::SMTFactory::createZ3Solver();
solver->assertExpr(expr);
auto result = solver->check();
```
