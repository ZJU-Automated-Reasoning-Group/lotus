# SLOT

SMT-Lightweight Optimization Translator.

## Overview

SLOT translates LLVM IR into SMT formulas for verification and analysis.

## Components

- **LLVMFunction** – Function-level translation
- **LLVMNode** – LLVM value to SMT node mapping
- **SMTFormula** – SMT formula construction
- **SMTNode** – SMT node representation

## Usage

```cpp
#include "Solvers/SMT/SLOT/LLVMFunction.h"

lotus::smt::slot::LLVMFunction translator;
auto formula = translator.translate(&function);
```
