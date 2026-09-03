PDG Query Language (Cypher)
============================

The Program Dependence Graph (PDG) Query Language uses Cypher, a declarative graph query language, to query dependencies, perform slicing, and verify security policies on program dependence graphs.

Overview
--------

.. note::

   The Cypher query rule set was expanded in a recent update
   (commit 3217fac4) with additional rules for pattern matching over
   the PDG IR. New security-focused query templates are available in
   ``tools/ir/examples/security/``.

Cypher provides a declarative way to:

- Query program dependencies (data and control)
- Perform forward and backward slicing
- Check information flow properties
- Verify security policies
- Find shortest paths between program elements
- Analyze parameter dependencies

The language supports:

- Pattern matching (nodes and relationships)
- WHERE clauses for filtering
- Path queries with variable-length patterns
- Set operations (UNION, intersection via WHERE EXISTS)
- Aggregations and ordering

Getting Started
---------------

Building the PDG
~~~~~~~~~~~~~~~~

First, compile your program and build the PDG:

.. code-block:: bash

   # Compile to LLVM IR
   clang -emit-llvm -g -c program.c -o program.bc
   
   # Run PDG query tool
   ./build/bin/lotus-ir-pdg-query program.bc

Interactive Mode
~~~~~~~~~~~~~~~~

Use ``-i`` flag for interactive queries:

.. code-block:: bash

   ./build/bin/lotus-ir-pdg-query -i program.bc
   
   PDG> MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n
   [Results displayed]
   
   PDG> MATCH (input:FUNC_ENTRY)-[:PARAMETER_OUT]->(ret:INST_RET)-[*]->(n)
   PDG> WHERE input.name = 'getInput'
   PDG> RETURN n
   [Slice displayed]
   
   PDG> exit

Single Query Mode
~~~~~~~~~~~~~~~~~

Use ``-q`` flag for single query:

.. code-block:: bash

   ./build/bin/lotus-ir-pdg-query -q "MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n" program.bc

Batch Query Mode
~~~~~~~~~~~~~~~~

Use ``-f`` flag to run queries from file:

.. code-block:: bash

   ./build/bin/lotus-ir-pdg-query -f queries.txt program.bc

Language Reference
------------------

Basic Node Queries
~~~~~~~~~~~~~~~~~~

Get All Nodes
^^^^^^^^^^^^^

Returns all nodes in the PDG.

**Syntax**: ``MATCH (n) RETURN n``

**Example**:

.. code-block:: cypher

   MATCH (n) RETURN n

**Returns**: All nodes in the PDG.

Query Nodes by Type
^^^^^^^^^^^^^^^^^^^

Select nodes by their label (type).

**Syntax**: ``MATCH (n:LABEL) RETURN n``

**Node Types (Labels)**:

- ``INST_FUNCALL`` - Function call instructions
- ``INST_RET`` - Return instructions
- ``INST_BR`` - Branch instructions
- ``INST_LOAD`` - Load instructions
- ``INST_STORE`` - Store instructions
- ``INST_ALLOCA`` - Alloca instructions
- ``INST_GEP`` - GetElementPtr instructions
- ``INST_OTHER`` - Other instructions
- ``FUNC_ENTRY`` - Function entry points
- ``PARAM_FORMALIN`` - Formal input parameters
- ``PARAM_FORMALOUT`` - Formal output parameters (return values)
- ``PARAM_ACTUALIN`` - Actual input parameters
- ``PARAM_ACTUALOUT`` - Actual output parameters
- ``GLOBAL`` - Global variables
- ``FUNC`` - Function nodes
- ``CLASS`` - Class nodes

**Example**:

.. code-block:: cypher

   MATCH (n:INST_FUNCALL) RETURN n
   MATCH (n:INST_BR) RETURN n

Function-Specific Queries
~~~~~~~~~~~~~~~~~~~~~~~~~~

Return Values of a Function
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Returns the return values of a function.

**Syntax**: ``MATCH (n:FUNC_ENTRY)-[:PARAMETER_OUT]->(ret:INST_RET) WHERE n.name = 'functionName' RETURN ret``

**Example**:

.. code-block:: cypher

   MATCH (n:FUNC_ENTRY)-[:PARAMETER_OUT]->(ret:INST_RET)
   WHERE n.name = 'main'
   RETURN ret

   MATCH (n:FUNC_ENTRY)-[:PARAMETER_OUT]->(ret:INST_RET)
   WHERE n.name = 'getInput'
   RETURN ret

**Returns**: Nodes representing return values of the specified function.

Formal Parameters of a Function
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Returns the formal parameters of a function.

**Syntax**: ``MATCH (n:FUNC_ENTRY)-[:PARAMETER_IN]->(param:PARAM_FORMALIN) WHERE n.name = 'functionName' RETURN param``

**Example**:

.. code-block:: cypher

   MATCH (n:FUNC_ENTRY)-[:PARAMETER_IN]->(param:PARAM_FORMALIN)
   WHERE n.name = 'process_data'
   RETURN param

**Returns**: Nodes representing formal input parameters.

Function Entry Points
^^^^^^^^^^^^^^^^^^^^^^

Returns the entry points of a function.

**Syntax**: ``MATCH (n:FUNC_ENTRY) WHERE n.name = 'functionName' RETURN n``

**Example**:

.. code-block:: cypher

   MATCH (n:FUNC_ENTRY)
   WHERE n.name = 'main'
   RETURN n

**Returns**: Function entry nodes.

Edge Queries
~~~~~~~~~~~~

Query Edges by Type
^^^^^^^^^^^^^^^^^^^

Select edges by their relationship type.

**Syntax**: ``MATCH ()-[r:TYPE]->() RETURN r``

**Edge Types (Relationship Types)**:

- ``DATA_DEF_USE`` - Data definition-use edges
- ``DATA_RAW`` - Read-after-write edges
- ``DATA_READ`` - Read edges
- ``DATA_WRITE`` - Write edges
- ``DATA_ALIAS`` - Alias edges
- ``CONTROL_DEP`` - Control dependency edges
- ``CONTROLDEP_BR`` - Control dependency from branches
- ``CONTROLDEP_ENTRY`` - Control dependency from entry
- ``PARAMETER_IN`` - Parameter input edges
- ``PARAMETER_OUT`` - Parameter output edges
- ``PARAMETER_FIELD`` - Field parameter edges
- ``CALL_DEP`` - Call dependency edges
- ``GLOBAL_DEP`` - Global dependency edges

**Example**:

.. code-block:: cypher

   MATCH ()-[r:DATA_DEF_USE]->() RETURN r
   MATCH ()-[r:CONTROLDEP_BR]->() RETURN r

Connected Nodes
^^^^^^^^^^^^^^^

Get nodes connected by edges.

**Syntax**: ``MATCH (a)-[r]->(b) RETURN a, b``

**Example**:

.. code-block:: cypher

   MATCH (a)-[r:DATA_DEF_USE]->(b) RETURN a, b
   MATCH (a)-[r:CONTROLDEP_BR]->(b) RETURN a, b

Slicing Operations
~~~~~~~~~~~~~~~~~~

Forward Slice
^^^^^^^^^^^^^

Compute forward slice from given nodes (all nodes reachable from source).

**Syntax**: ``MATCH (source)-[*]->(n) RETURN DISTINCT n``

**Example**:

.. code-block:: cypher

   MATCH (input:FUNC_ENTRY)-[:PARAMETER_OUT]->(inputRet:INST_RET)
   WHERE input.name = 'getInput'
   MATCH (inputRet)-[*]->(n)
   RETURN DISTINCT n

**Returns**: All nodes reachable from the input nodes.

**Use Case**: Find what program elements are affected by a source.

Backward Slice
^^^^^^^^^^^^^^

Compute backward slice from given nodes (all nodes that can reach the sink).

**Syntax**: ``MATCH (n)-[*]->(sink) RETURN DISTINCT n``

**Example**:

.. code-block:: cypher

   MATCH (output:FUNC_ENTRY)-[:PARAMETER_IN]->(outputParam:PARAM_FORMALIN)
   WHERE output.name = 'printOutput'
   MATCH (n)-[*]->(outputParam)
   RETURN DISTINCT n

**Returns**: All nodes that can reach the input nodes.

**Use Case**: Find what program elements influence a sink.

Path Between Nodes
^^^^^^^^^^^^^^^^^^

Find nodes on paths between two sets of nodes.

**Syntax**: ``MATCH path = (start)-[*]->(end) RETURN path``

**Example**:

.. code-block:: cypher

   MATCH (input:FUNC_ENTRY)-[:PARAMETER_OUT]->(inputRet:INST_RET)
   WHERE input.name = 'getInput'
   MATCH (output:FUNC_ENTRY)-[:PARAMETER_IN]->(outputParam:PARAM_FORMALIN)
   WHERE output.name = 'system'
   MATCH path = (inputRet)-[*]->(outputParam)
   RETURN path

**Returns**: Paths from sources to sinks.

**Use Case**: Information flow analysis.

Shortest Path
^^^^^^^^^^^^^

Find shortest path between two sets of nodes.

**Syntax**: ``MATCH path = shortestPath((start)-[*]->(end)) RETURN path``

**Example**:

.. code-block:: cypher

   MATCH (malloc:FUNC_ENTRY)-[:PARAMETER_OUT]->(mallocRet:INST_RET)
   WHERE malloc.name = 'malloc'
   MATCH (free:FUNC_ENTRY)-[:PARAMETER_IN]->(freeParam:PARAM_FORMALIN)
   WHERE free.name = 'free'
   MATCH path = shortestPath((mallocRet)-[*]->(freeParam))
   RETURN path

**Returns**: Shortest path between nodes.

Set Operations
~~~~~~~~~~~~~~~

Union
^^^^^

Union of two sets using ``UNION`` keyword.

**Syntax**: ``MATCH ... RETURN ... UNION MATCH ... RETURN ...``

**Example**:

.. code-block:: cypher

   MATCH (func1:FUNC_ENTRY)-[:PARAMETER_OUT]->(ret1:INST_RET)
   WHERE func1.name = 'func1'
   RETURN ret1
   UNION
   MATCH (func2:FUNC_ENTRY)-[:PARAMETER_OUT]->(ret2:INST_RET)
   WHERE func2.name = 'func2'
   RETURN ret2

**Returns**: All nodes in either set.

Intersection
^^^^^^^^^^^^

Intersection using ``WHERE EXISTS`` subquery.

**Syntax**: ``MATCH (n) WHERE EXISTS { MATCH (m) WHERE ... } RETURN n``

**Example**:

.. code-block:: cypher

   MATCH (dataNodes:INST_FUNCALL)
   WHERE EXISTS {
     MATCH (controlNodes:INST_BR)
     WHERE dataNodes.id = controlNodes.id
   }
   RETURN dataNodes

**Returns**: Nodes in both sets.

**Use Case**: Find nodes that are both data and control dependent.

Difference
^^^^^^^^^^

Set difference using ``WHERE NOT``.

**Syntax**: ``MATCH (n) WHERE NOT (condition) RETURN n``

**Example**:

.. code-block:: cypher

   MATCH (calls:INST_FUNCALL)
   WHERE NOT (calls:INST_BR)
   RETURN calls

**Returns**: Nodes in first set but not matching condition.

Filtering with WHERE
~~~~~~~~~~~~~~~~~~~~

Property Filtering
^^^^^^^^^^^^^^^^^^

Filter nodes by properties.

**Syntax**: ``MATCH (n) WHERE n.property = value RETURN n``

**Example**:

.. code-block:: cypher

   MATCH (n:FUNC_ENTRY)
   WHERE n.name = 'main'
   RETURN n

   MATCH (n)
   WHERE n.line > 100 AND n.line < 200
   RETURN n

Multiple Conditions
^^^^^^^^^^^^^^^^^^^

Combine conditions with AND, OR, NOT.

**Syntax**: ``MATCH (n) WHERE condition1 AND condition2 RETURN n``

**Example**:

.. code-block:: cypher

   MATCH (n:INST_FUNCALL)
   WHERE n.name = 'malloc' OR n.name = 'calloc'
   RETURN n

Security Analysis Examples
--------------------------

Information Flow Analysis
~~~~~~~~~~~~~~~~~~~~~~~~~

Check if secret data flows to network:

.. code-block:: cypher

   MATCH (secret:FUNC_ENTRY)-[:PARAMETER_OUT]->(secretRet:INST_RET)
   WHERE secret.name = 'getPassword'
   MATCH (network:FUNC_ENTRY)-[:PARAMETER_IN]->(networkParam:PARAM_FORMALIN)
   WHERE network.name = 'sendToNetwork'
   MATCH path = (secretRet)-[*]->(networkParam)
   RETURN path

If result is non-empty, there's a potential information leak.

Sanitization Verification
~~~~~~~~~~~~~~~~~~~~~~~~~

Verify all user inputs are sanitized before use in SQL:

.. code-block:: cypher

   MATCH (sources:FUNC_ENTRY)-[:PARAMETER_OUT]->(sourceRet:INST_RET)
   WHERE sources.name = 'getUserInput'
   MATCH (sanitizers:FUNC_ENTRY)-[:PARAMETER_OUT]->(sanitizerRet:INST_RET)
   WHERE sanitizers.name = 'sanitizeSQL'
   MATCH (sinks:FUNC_ENTRY)-[:PARAMETER_IN]->(sinkParam:PARAM_FORMALIN)
   WHERE sinks.name = 'executeSQL'
   MATCH path = (sourceRet)-[*]->(sinkParam)
   WHERE NOT EXISTS {
     MATCH (sourceRet)-[*]->(sanitizerRet)-[*]->(sinkParam)
   }
   RETURN path

If result is empty, all flows are sanitized.

Access Control Policy
~~~~~~~~~~~~~~~~~~~~~~

Verify sensitive file operations require authorization:

.. code-block:: cypher

   MATCH (auth:FUNC_ENTRY)-[:PARAMETER_OUT]->(authRet:INST_RET)
   WHERE auth.name = 'checkPermission'
   MATCH (authRet)-[:CONTROLDEP_BR]->(check)
   MATCH (fileOps:FUNC_ENTRY)-[:PARAMETER_IN]->(fileParam:PARAM_FORMALIN)
   WHERE fileOps.name = 'openFile' OR fileOps.name = 'writeFile'
   WHERE NOT EXISTS {
     MATCH (check)-[:CONTROLDEP_BR]->(fileParam)
   }
   RETURN fileParam

Null Pointer Analysis
~~~~~~~~~~~~~~~~~~~~~

Find potential null dereferences:

.. code-block:: cypher

   MATCH (nullRet:INST_RET)
   WHERE nullRet.function = 'malloc' OR nullRet.function = 'fopen'
   MATCH (nullRet)-[*]->(deref)
   WHERE deref:INST_LOAD OR deref:INST_STORE
   RETURN deref

Control Flow Analysis
~~~~~~~~~~~~~~~~~~~~~

Find which conditionals affect a computation:

.. code-block:: cypher

   MATCH (branches:INST_BR)
   MATCH (computation:FUNC_ENTRY)-[:PARAMETER_IN]->(compParam:PARAM_FORMALIN)
   WHERE computation.name = 'compute'
   MATCH (branches)-[*]->(compParam)
   RETURN branches

Parameter Dependency
~~~~~~~~~~~~~~~~~~~~

Find dependencies between function parameters:

.. code-block:: cypher

   MATCH (inputs:FUNC_ENTRY)-[:PARAMETER_IN]->(inputParam:PARAM_FORMALIN)
   WHERE inputs.name = 'processData'
   MATCH (outputs:FUNC_ENTRY)-[:PARAMETER_OUT]->(outputRet:INST_RET)
   WHERE outputs.name = 'processData'
   MATCH path = (inputParam)-[*]->(outputRet)
   RETURN path

Taint Analysis
~~~~~~~~~~~~~~

Complete taint analysis from sources to sinks:

.. code-block:: cypher

   MATCH (sources:FUNC_ENTRY)-[:PARAMETER_OUT]->(sourceRet:INST_RET)
   WHERE sources.name = 'read' OR sources.name = 'recv' OR sources.name = 'scanf'
   MATCH (sinks:FUNC_ENTRY)-[:PARAMETER_IN]->(sinkParam:PARAM_FORMALIN)
   WHERE sinks.name = 'system' OR sinks.name = 'exec' OR sinks.name = 'popen'
   MATCH path = (sourceRet)-[*]->(sinkParam)
   RETURN path

Query File Format
-----------------

Batch query files support:

- Comments (``#`` prefix)
- Multiple queries (one per line or separated by ``;``)
- Multi-line queries

Example query file (``security_policy.txt``):

.. code-block:: cypher

   # Security Policy Verification
   
   # Check for direct flows from sources to sinks
   MATCH (sources:FUNC_ENTRY)-[:PARAMETER_OUT]->(sourceRet:INST_RET)
   WHERE sources.name = 'read' OR sources.name = 'recv'
   MATCH (sinks:FUNC_ENTRY)-[:PARAMETER_IN]->(sinkParam:PARAM_FORMALIN)
   WHERE sinks.name = 'system' OR sinks.name = 'exec'
   MATCH path = (sourceRet)-[*]->(sinkParam)
   RETURN path
   
   # Check sensitive operations are authorized
   MATCH (auth:FUNC_ENTRY)-[:PARAMETER_OUT]->(authRet:INST_RET)
   WHERE auth.name = 'checkAuth'
   MATCH (authRet)-[:CONTROLDEP_BR]->(check)
   MATCH (sensitiveOps:FUNC_ENTRY)-[:PARAMETER_IN]->(opParam:PARAM_FORMALIN)
   WHERE sensitiveOps.name = 'deleteFile' OR sensitiveOps.name = 'changePassword'
   WHERE NOT EXISTS {
     MATCH (check)-[:CONTROLDEP_BR]->(opParam)
   }
   RETURN opParam

Run with:

.. code-block:: bash

   ./build/bin/lotus-ir-pdg-query -f security_policy.txt program.bc

Best Practices
--------------

1. **Use Descriptive Variable Names**:

   .. code-block:: cypher

      # Good
      MATCH (userInput:FUNC_ENTRY)-[:PARAMETER_OUT]->(inputRet:INST_RET)
      WHERE userInput.name = 'scanf'
      
      # Bad
      MATCH (x:FUNC_ENTRY)-[:PARAMETER_OUT]->(y:INST_RET)
      WHERE x.name = 'scanf'

2. **Break Complex Queries**:

   Use multiple MATCH clauses for clarity:

   .. code-block:: cypher

      MATCH (sources:FUNC_ENTRY)-[:PARAMETER_OUT]->(sourceRet:INST_RET)
      WHERE sources.name = 'getInput'
      MATCH (sanitizers:FUNC_ENTRY)-[:PARAMETER_OUT]->(sanitizerRet:INST_RET)
      WHERE sanitizers.name = 'sanitize'
      MATCH (sinks:FUNC_ENTRY)-[:PARAMETER_IN]->(sinkParam:PARAM_FORMALIN)
      WHERE sinks.name = 'output'
      MATCH path = (sourceRet)-[*]->(sinkParam)
      WHERE NOT EXISTS {
        MATCH (sourceRet)-[*]->(sanitizerRet)-[*]->(sinkParam)
      }
      RETURN path

3. **Comment Your Policies**:

   .. code-block:: cypher

      # Policy: User input must be sanitized before database queries
      MATCH (input:FUNC_ENTRY)-[:PARAMETER_OUT]->(inputRet:INST_RET)
      WHERE input.name = 'getUserInput'
      MATCH (sanitize:FUNC_ENTRY)-[:PARAMETER_OUT]->(sanitizerRet:INST_RET)
      WHERE sanitize.name = 'sanitizeSQL'
      MATCH (dbQuery:FUNC_ENTRY)-[:PARAMETER_IN]->(queryParam:PARAM_FORMALIN)
      WHERE dbQuery.name = 'executeQuery'
      MATCH path = (inputRet)-[*]->(queryParam)
      WHERE NOT EXISTS {
        MATCH (inputRet)-[*]->(sanitizerRet)-[*]->(queryParam)
      }
      RETURN path

4. **Test Incrementally**:

   Start with simple queries and build up:

   .. code-block:: cypher

      # Step 1: Verify sources exist
      MATCH (n:FUNC_ENTRY) WHERE n.name = 'getInput' RETURN n
      
      # Step 2: Verify sinks exist
      MATCH (n:FUNC_ENTRY) WHERE n.name = 'system' RETURN n
      
      # Step 3: Check flows
      MATCH (input:FUNC_ENTRY)-[:PARAMETER_OUT]->(inputRet:INST_RET)
      WHERE input.name = 'getInput'
      MATCH (output:FUNC_ENTRY)-[:PARAMETER_IN]->(outputParam:PARAM_FORMALIN)
      WHERE output.name = 'system'
      MATCH path = (inputRet)-[*]->(outputParam)
      RETURN path

5. **Use DISTINCT for Slices**:

   .. code-block:: cypher

      MATCH (start)-[*]->(n)
      RETURN DISTINCT n

Limitations
-----------

1. **Imprecision**: PDG construction uses alias analysis which may be imprecise, leading to spurious dependencies.

2. **Scalability**: Large programs with complex dependencies may result in very large PDGs.

3. **Context Sensitivity**: PDG is context-insensitive by default. Multiple calls to the same function are merged.

4. **Array Handling**: Array dependencies may be approximated.

5. **Pointer Analysis**: Depends on underlying pointer analysis precision.

Performance Tips
---------------

1. **Limit Path Length**: Use bounded variable-length patterns when possible:

   .. code-block:: cypher

      MATCH (start)-[*1..5]->(end)
      RETURN path

2. **Use Specific Labels**: Always specify node labels when possible:

   .. code-block:: cypher

      # Good
      MATCH (n:INST_FUNCALL) RETURN n
      
      # Less efficient
      MATCH (n) WHERE n:INST_FUNCALL RETURN n

3. **Filter Early**: Apply WHERE clauses as early as possible:

   .. code-block:: cypher

      MATCH (n:FUNC_ENTRY)
      WHERE n.name = 'main'
      MATCH (n)-[:PARAMETER_OUT]->(ret:INST_RET)
      RETURN ret

Troubleshooting
---------------

Query Returns Empty Set
~~~~~~~~~~~~~~~~~~~~~~~~

1. Check function names are correct (case-sensitive)
2. Verify PDG was built successfully
3. Try simpler queries first (``MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n``)
4. Use ``-v`` verbose flag

Query Too Slow
~~~~~~~~~~~~~~

1. Reduce scope of query
2. Use more specific node/edge selection
3. Limit path length with ``[*1..10]``
4. Analyze smaller program subset

Syntax Errors
~~~~~~~~~~~~~

1. Check quotes around string values: ``WHERE n.name = 'main'``
2. Ensure proper Cypher syntax
3. Balance parentheses and brackets
4. Check relationship types are correct

See Also
--------

- :doc:`../ir/pdg` - PDG construction details
- :doc:`tutorials` - PDG usage examples
- :doc:`../developer/api_reference` - Programmatic PDG access
- ``tools/ir/examples/`` - In-repo example query cookbook
- :doc:`../tools/ir/examples` - Rendered documentation for the example queries
