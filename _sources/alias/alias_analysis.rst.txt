Alias Analysis Components
==========================

Lotus provides several alias analysis algorithms with different precision/performance trade-offs. Each analysis makes different trade-offs between precision, scalability, and analysis cost.

Reusable support modules are documented separately:

- :doc:`metrics` for evaluation helpers
- :doc:`ptsset` for points-to set backends
- :doc:`spec` for external-library specification support

Analysis Selection Guide
-------------------------

Choose the right analysis for your needs:

**For Large Codebases (Speed Priority)**:

- **SparrowAA (CI mode)**: Fastest, context-insensitive, inclusion-based
- **AserPTA (CI mode)**: Fast with field sensitivity option
- **AllocAA**: Lightweight heuristic-based tracking
- **FPA (KELP)**: Specialized for function pointer resolution
- **SRAA**: Range-based for proving non-aliasing
- **DyckAA**: Unification-based with Dyck-CFL reachability

**For Maximum Precision**:

- **LotusAA**: Flow-sensitive and field-sensitive
- **FlowSensitivePTA**: Sparse flow-sensitive, inclusion-based
- **Sea-DSA**: Unification-based, context-sensitive with heap cloning
- **SparrowAA (1-CFA, 2-CFA)**: Context-sensitive, inclusion-based
- **AserPTA (1-CFA, 2-CFA, Origin)**: Context-sensitive, inclusion-based

Available Analyses
------------------

For detailed information about each analysis, see the corresponding documentation:

* :doc:`allocaa` - Lightweight heuristic-based alias analysis
* :doc:`aserpta` - High-performance pointer analysis with multiple context sensitivities
* :doc:`dyckaa` - Unification-based alias analysis with Dyck-CFL reachability
* :doc:`seadsa` - Context-sensitive, field-sensitive alias analysis based on DSA
* :doc:`sparrowaa` - Inclusion-based points-to analysis
* :doc:`fpa` - Function pointer analysis with multiple algorithms
* :doc:`lotusaa` - Lotus-specific alias analysis framework
* :doc:`underapproxaa` - Under-approximate alias analysis for conservative results
* :doc:`dynaa` - Dynamic validation of static alias analysis results
* :doc:`sraa` - Strict Relation Alias Analysis built on interprocedural range analysis
* :doc:`flowsensitive` - Sparse flow-sensitive inclusion-based pointer analysis
