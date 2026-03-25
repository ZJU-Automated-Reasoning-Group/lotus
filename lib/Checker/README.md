# Checker Library

Bug checkers and reporting infrastructure.

**Status**: Most checkers are unstable; many were migrated from other projects (notably SVF) and may require further integration work.

| Subdir | Purpose | Status |
|--------|---------|--------|
| **AE** | Abstract Execution (abstract interpretation engine). Migrated from SVF's AE engine. | ⚠️ Migrated (from SVF), unstable |
| **Concurrency** | Thread-safety: Atomicity, ConditionVariable, DataRace, Deadlock, LockMismatch. | ⚠️ Unstable |
| **FiTx** | Detectors: double-free, double-lock/unlock, leak, null-ptr, ref/ unref, UAF, use-before-init. | ✅ Stable |
| **KINT** | Integer bug detection. Taint analysis, SMT (Z3). | ⚠️ Unstable |
| **Pulse** | Biabductive analysis (Infer Pulse–style). Witnessable bugs, disjunctive domain, loop abstraction. | ⚠️ Migrated (from Infer), Unstable |
| **Saber** | Source-sink bug detector. Migrated from SVF's SABER engine. Checkers: memory leak, double-free, file operations (fopen/fclose). | ⚠️ Migrated (from SVF), unstable |
| **Report** | Shared reporting: BugReport, BugReportMgr, BugTypes, SARIF, SuppressionManager. | ✅ Stable |
