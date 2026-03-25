Lotus: Program Analysis Framework
==================================

Lotus is a comprehensive program analysis, verification, and optimization framework built on LLVM. It provides multiple toolkits that can be used individually or in combination for various static analysis tasks.



.. toctree::
   :maxdepth: 2
   :caption: User Guide

   user_guide/major_components
   user_guide/architecture
   user_guide/quickstart
   user_guide/installation
   user_guide/tutorials
   user_guide/bug_detection
   user_guide/pdg_query_language
   user_guide/property_based_slicing
   user_guide/verification_backends
   user_guide/instrumentation_passes
   user_guide/troubleshooting
   tools/index

.. toctree::
   :maxdepth: 2
   :caption: Core Components

   alias/index
   analysis/index
   annotation/index
   apps/index
   cfl/index
   dataflow/index
   ir/index
   ml/index
   optimization/index
   solvers/index
   transform/index
   utils/index
   verification/index
   checker/index

.. toctree::
   :maxdepth: 2
   :caption: Developer Documentation

   developer/api_reference
   developer/developer_guide

Features
--------

* **Multiple Alias Analysis Algorithms**: DyckAA, Sea-DSA, SparrowAA, AserPTA, FPA, OriginAA (CFL via LLVM)
* **Dynamic Analysis Validation**: DynAA for validating static analysis results
* **Intermediate Representations**: PDG, SVFG, DyckVFG
* **Constraint Solving**: SMT (Z3), BDD (CUDD), WPDS
* **Data Flow Analysis**: IFDS/IDE framework, taint analysis
* **Bug Detection**: Integer overflow, null pointer, buffer overflow detection
* **LLVM Integration**: Built on LLVM 14 with comprehensive IR support

Supported Platforms
-------------------

* x86 Linux
* ARM Mac
* LLVM 14.0.0
* Z3 4.11

Publications
------------

* **ISSTA 2025**: *Program Analysis Combining Generalized Bit-Level and Word-Level Abstractions*  
  Guangsheng Fan, Liqian Chen, Banghu Yin, Wenyu Zhang, Peisen Yao, and Ji Wang.  
  *The ACM SIGSOFT International Symposium on Software Testing and Analysis*.

* **S&P 2024**: *Titan: Efficient Multi-target Directed Greybox Fuzzing*  
  Heqing Huang, Peisen Yao, Hung-Chun Chiu, Yiyuan Guo, and Charles Zhang.

* **USENIX Security 2024**: *Unleashing the Power of Type-Based Call Graph Construction by Using Regional Pointer Information*  
  Yuandao Cai, Yibo Jin, and Charles Zhang.

* **TSE 2024**: *Fast and Precise Static Null Exception Analysis with Synergistic Preprocessing*  
  Yi Sun, Chengpeng Wang, Gang Fan, Qingkai Shi, and Xiangyu Zhang.

* **OOPSLA 2022**: *Indexing the Extended Dyck-CFL Reachability for Context-Sensitive Program Analysis*  
  Qingkai Shi, Yongchao Wang, Peisen Yao, and Charles Zhang.



Indices and tables
==================

* :ref:`genindex`
* :ref:`modindex`
* :ref:`search`
