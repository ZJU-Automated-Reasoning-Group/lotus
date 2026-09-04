# Vendored verification backends

This directory contains upstream-derived CLAM, SeaHorn, and SMACK code. Lotus
keeps these sources separate from its owned verification infrastructure under
`include/Verification/` and `lib/Verification/`.

Each backend preserves the historical `Verification/<backend>/...` include
spelling beneath its own `include/` directory. The Lotus-owned build adapters in
`lib/Verification/Backends/` add these trees only when the matching
`LOTUS_ENABLE_*` option is enabled.

Local integration changes should be kept minimal and documented in the backend
subtree. New Lotus APIs should not be added here.
