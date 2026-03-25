# AliasAnalysisWrapper – Configuration-Based Design

## Overview

`AliasAnalysisWrapper` uses a **configuration-based** approach (not a flat enum): explicit implementation choice, context-sensitivity settings, type safety, and extensibility.

## Architecture

### AAConfig

```cpp
struct AAConfig {
  Implementation impl;           // Which analysis
  ContextSensitivity ctxSens;    // Context-sensitivity mode
  unsigned kLimit;               // k-CFA limit (0 = CI)
  bool fieldSensitive;
  Solver solver;                 // AserPTA solver
};
```

### Implementations

- **SparrowAA**: Andersen-style inclusion-based
- **AserPTA**: High-performance (multiple solvers)
- **TPA**: Flow- and context-sensitive semi-sparse
- **DyckAA**, **CFLAnders/CFLSteens**: CFL-reachability
- **SeaDsa**: Unification-based | **AllocAA**: Allocation heuristic | **UnderApprox** | **Combined**

## Usage

**IMPORTANT**: Users MUST explicitly specify which analysis to use. There are no default parameters - this ensures users are aware of what analysis is running and its performance/precision implications.

### Basic / TPA / AserPTA

```cpp
// SparrowAA: NoCtx, 1-CFA, 2-CFA
// NOTE: Explicit specification required - no defaults!
auto aa1 = AliasAnalysisFactory::create(M, AAConfig::SparrowAA_NoCtx());
auto aa2 = AliasAnalysisFactory::create(M, AAConfig::SparrowAA_1CFA());
auto aa3 = AliasAnalysisFactory::create(M, AAConfig::SparrowAA_2CFA());

// Direct constructor also requires explicit config
AliasAnalysisWrapper wrapper(M, AAConfig::SparrowAA_NoCtx());

// TPA: NoCtx, 1-CFA, 2-CFA, custom k
auto aa4 = AliasAnalysisFactory::create(M, AAConfig::TPA_NoCtx());
auto aa5 = AliasAnalysisFactory::create(M, AAConfig::TPA_KCFA(5));

// AserPTA: NoCtx, 1-CFA (Deep), origin-sensitive
auto aa6 = AliasAnalysisFactory::create(M, AAConfig::AserPTA_NoCtx());
auto aa7 = AliasAnalysisFactory::create(M, AAConfig::AserPTA_1CFA(AAConfig::Solver::Deep));
auto aa8 = AliasAnalysisFactory::create(M, AAConfig::AserPTA_Origin());
```

### Convenience & Custom

```cpp
// Quick k-CFA (still explicit - just shorter syntax)
auto aa = AliasAnalysisFactory::createSparrowAA(M, 1);
auto aa = AliasAnalysisFactory::createAserPTA(M, 2);
auto aa = AliasAnalysisFactory::createTPA(M, 3);

// Custom AAConfig (explicit construction)
AAConfig c; c.impl = AAConfig::Implementation::TPA;
c.ctxSens = AAConfig::ContextSensitivity::KCallSite;
c.kLimit = 4; c.fieldSensitive = true;
auto aa = AliasAnalysisFactory::create(M, c);
```

### Why Explicit Specification?

- **Awareness**: Users know exactly what analysis is running
- **Performance**: Different analyses have vastly different performance characteristics
- **Precision**: Different analyses provide different levels of precision
- **Debugging**: Makes it easier to understand results and debug issues
- **Reproducibility**: Explicit configs make results reproducible and documentable

## Migration

| Old (deprecated) | New |
|------------------|-----|
| `AAType::Andersen` | `AAConfig::SparrowAA_NoCtx()` |
| `AAType::Andersen1CFA` | `AAConfig::SparrowAA_1CFA()` |
| `AAType::TPA` | `AAConfig::TPA_2CFA()` (or other TPA preset) |

## Benefits

- **Clarity**: `SparrowAA_1CFA()` vs `Andersen1CFA`
- **Distinction**: `SparrowAA_1CFA()` vs `AserPTA_1CFA()` – implementation is explicit
- **Flexibility**: Add parameters (field sensitivity, solver) without enum bloat
- **Type safety**: Valid configs checked at compile time

## File Layout

- **AliasAnalysisWrapperCore.cpp**: Constructors, init
- **AliasAnalysisWrapperQueries.cpp**: Public query API
- **AliasAnalysisWrapperBackend.cpp**: Backend routing
- **AliasAnalysisFactory.cpp**: Factory and helpers

## Current Limitations

- **AserPTA**: The API accepts `AserPTA_*` configs, but the wrapper backend does not execute a fallback analysis anymore. It now fails initialization explicitly until full AserPTA integration is implemented.
- Field-sensitivity toggles, adaptive context sensitivity (TPA)
- More AserPTA solver options, config validation
