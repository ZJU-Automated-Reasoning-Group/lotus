Failure-Directed Trimming
=========================

``FailureDirectedTrimming`` contains verification support for reducing programs
or paths around observed failure behavior.

**Headers**: ``include/Verification/Transform/FailureDirectedTrimming/``

**Implementation**: ``lib/Verification/Transform/FailureDirectedTrimming/``

Overview
--------

This subsystem provides internal support for trimming verification problems so
backends can focus on failure-relevant behavior. It is currently infrastructure
code rather than a documented standalone end-user tool.

Use cases
---------

- Reduce search space around failure witnesses.
- Support verification backends that benefit from smaller failure-focused IR.
- Provide reusable transforms for experimental verification pipelines.

Notes
-----

The current documentation is intentionally high level because this subsystem is
not yet exposed through a dedicated stable front-end.

Integration considerations
--------------------------

Apply trimming only after a pipeline has identified the failure information it
wants to preserve.  The reduced IR is useful for focused exploration and
debugging, but it should not be assumed equivalent to the original module for
unrelated properties.  Experimental clients should retain the original module
and validate any witness against it.
