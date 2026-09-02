# Legacy LLVM fixtures

Historical source-to-LLVM fixture generators spanning several Lotus modules.
This directory is retained as a migration boundary because its original root
CMake hierarchy is not wired into the normal test entry point.

Move an active subset to `tests/regress/<OwningModule>/` when its consumer and
oracle are known. Do not add new fixtures here.
