# Verification backend adapters

This directory is the Lotus-owned integration boundary for optional verification
backends. Its CMake adapter selects and builds the vendored CLAM, SeaHorn, and
SMACK sources from `third-party/verification/`.

Do not place imported backend sources here. Lotus-specific wrappers and stable
backend-neutral APIs belong here; upstream-derived implementation and headers
belong under the corresponding `third-party/verification/<backend>/` tree.
