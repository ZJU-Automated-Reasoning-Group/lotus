# Legacy LLVM fixtures

Historical source-to-LLVM fixture generators spanning several Lotus modules.
This directory is archived because its original root CMake hierarchy is not
wired into the normal test entry point and no current consumers were found.

Move an active subset to `tests/regress/<OwningModule>/` when its consumer and
oracle are known. Do not add new fixtures here. The actively consumed type
hierarchy fixtures were migrated to `tests/regress/Analysis/TypeHierarchy/`.
