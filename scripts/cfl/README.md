# CFL parity tooling

`compare_svf_cfl.py` runs the Lotus bitcode alias frontend and an independently
built SVF `cfl` oracle on the same LLVM corpus. For textual PTATest cases it
translates the existing `__aser_alias__`/`__aser_no_alias__` annotations to
SVF's `MAYALIAS`/`NOALIAS` convention in a temporary directory. The JSON report
contains semantic annotation parity, solver metrics, wall time, failures, and
timeouts per case.

The SVF executable and grammar remain external inputs; they are never linked
into or redistributed with Lotus.
