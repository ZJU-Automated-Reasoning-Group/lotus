Tutorials and Examples
======================

This section provides hands-on tutorials for using Lotus to analyze real programs.
For optimization passes that can reshape IR before analysis, see
:doc:`../optimization/index` and the :doc:`../optimization/swprefetching` guide.

Tutorial 1: Basic Alias Analysis
---------------------------------

Let's analyze a simple C program to understand pointer relationships.

Example Program
~~~~~~~~~~~~~~~

Create ``example1.c``:

.. code-block:: c

   #include <stdlib.h>
   
   int *global_ptr;
   
   void update_value(int *p, int value) {
       *p = value;
   }
   
   int main() {
       int x = 10;
       int y = 20;
       int *ptr1 = &x;
       int *ptr2 = &y;
       
       update_value(ptr1, 30);
       
       int *heap = (int *)malloc(sizeof(int));
       *heap = 40;
       
       global_ptr = heap;
       
       free(heap);
       return 0;
   }

Compile to LLVM IR
~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   clang -emit-llvm -c -g example1.c -o example1.bc
   clang -emit-llvm -S -g example1.c -o example1.ll

Run Different Alias Analyses
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**1. SparrowAA (CI mode) Analysis (Fast)**:

.. code-block:: bash

   ./build/bin/lotus-alias-sparrow-aa example1.bc

This performs context-insensitive, inclusion-based pointer analysis. Good for quick scans.

Note: lotus-alias-sparrow-aa also has context-sensitive variants.

**2. AserPTA with 1-CFA (Balanced)**:

.. code-block:: bash

   ./build/bin/lotus-alias-aser-aa -analysis-mode=1-cfa -solver=wave -dump-stats example1.bc

This adds call-site sensitivity for better precision while maintaining good performance.

**3. DyckAA (Precise)**:

.. code-block:: bash

   ./build/bin/lotus-alias-dyck-aa -print-alias-set-info example1.bc

Produces the most precise alias sets using Dyck-CFL reachability.

Understanding Output
~~~~~~~~~~~~~~~~~~~~

The analyses will report:

- **Points-to sets**: What each pointer may point to
- **Alias sets**: Which pointers may alias each other
- **Call graph**: Which functions may be called through function pointers
- **Statistics**: Number of pointers, objects, constraints solved

Tutorial 2: Detecting Integer Overflow
---------------------------------------

Let's find integer overflow bugs using Kint.

Example Program
~~~~~~~~~~~~~~~

Create ``overflow.c``:

.. code-block:: c

   #include <stdio.h>
   #include <limits.h>
   
   int add_values(int a, int b) {
       return a + b;  // Potential overflow
   }
   
   int multiply_values(int a, int b) {
       return a * b;  // Potential overflow
   }
   
   int safe_add(int a, int b) {
       if (a > 0 && b > INT_MAX - a) {
           return INT_MAX;  // Saturate
       }
       if (a < 0 && b < INT_MIN - a) {
           return INT_MIN;  // Saturate
       }
       return a + b;  // Safe
   }
   
   int main() {
       int x = INT_MAX - 10;
       int y = 20;
       
       int result1 = add_values(x, y);  // Bug: overflow
       int result2 = multiply_values(1000, 3000000);  // Bug: overflow
       int result3 = safe_add(x, y);  // Safe
       
       printf("Results: %d, %d, %d\n", result1, result2, result3);
       return 0;
   }

Compile and Analyze
~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   clang -emit-llvm -S -g overflow.c -o overflow.ll
   ./build/bin/lotus-check --engine=kint overflow.ll --checks=int-overflow --kint.analyze-all-functions

Expected Output
~~~~~~~~~~~~~~~

The tool will report:

.. code-block:: text

   [Integer Overflow] Function: add_values
   Line 5: return a + b;
   Reason: Operands a and b may cause signed integer overflow
   
   [Integer Overflow] Function: multiply_values
   Line 9: return a * b;
   Reason: Operands a and b may cause signed integer overflow

Note that ``safe_add`` is not reported because the checks prevent overflow.

Advanced: All Checks
~~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint overflow.ll --checks=all --kint.analyze-all-functions

This enables all checkers: integer overflow, division by zero, bad shift, array bounds, and dead branches.

Tutorial 3: Null Pointer Detection
-----------------------------------

Let's detect null pointer dereferences.
This tutorial uses Pulse because it produces witness-oriented diagnostics;
``ae``, ``fitx``, and ``symex`` also support null-pointer checks with different
trade-offs.  See :ref:`Choosing a Checker <choosing-a-checker>` before choosing
an engine for a production workflow.

Example Program
~~~~~~~~~~~~~~~

Create ``nullpointer.c``:

.. code-block:: c

   #include <stdlib.h>
   #include <string.h>
   
   typedef struct {
       int id;
       char *name;
   } Person;
   
   Person *create_person(int id, const char *name) {
       if (id < 0) {
           return NULL;  // Error case
       }
       
       Person *p = (Person *)malloc(sizeof(Person));
       if (p) {
           p->id = id;
           p->name = strdup(name);
       }
       return p;
   }
   
   void print_person(Person *p) {
       printf("Person %d: %s\n", p->id, p->name);  // Bug if p is NULL
   }
   
   int main() {
       Person *p1 = create_person(1, "Alice");
       print_person(p1);  // Safe if malloc succeeded
       
       Person *p2 = create_person(-1, "Bob");
       print_person(p2);  // Bug: p2 is NULL
       
       Person *p3 = create_person(2, "Charlie");
       if (p3) {
           print_person(p3);  // Safe: null check
       }
       
       return 0;
   }

Compile and Analyze
~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   clang -emit-llvm -c -g nullpointer.c -o nullpointer.bc
   ./build/bin/lotus-check --engine=pulse nullpointer.bc --verbose

Expected Output
~~~~~~~~~~~~~~~

The analysis will report:

.. code-block:: text

   [Null Pointer Dereference]
   Function: print_person
   Line 24: printf("Person %d: %s\n", p->id, p->name);
   Path: create_person returns NULL → print_person dereferences p
   
   Call site in main (line 32) passes potentially NULL pointer

Tutorial 4: Taint Analysis
---------------------------

Let's track information flow from untrusted sources to sensitive sinks.

Example Program
~~~~~~~~~~~~~~~

Create ``taint.c``:

.. code-block:: c

   #include <stdio.h>
   #include <stdlib.h>
   #include <string.h>
   
   void execute_command(const char *cmd) {
       system(cmd);  // Dangerous sink
   }
   
   char *sanitize(const char *input) {
       // Simplified sanitization
       char *output = strdup(input);
       for (int i = 0; output[i]; i++) {
           if (output[i] == ';' || output[i] == '|') {
               output[i] = '_';
           }
       }
       return output;
   }
   
   int main() {
       char user_input[256];
       printf("Enter command: ");
       scanf("%255s", user_input);  // Tainted source
       
       // Bug: direct flow from source to sink
       execute_command(user_input);
       
       // Safe: sanitized before use
       char *safe_cmd = sanitize(user_input);
       execute_command(safe_cmd);
       free(safe_cmd);
       
       return 0;
   }

Compile and Analyze
~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   clang -emit-llvm -c -g taint.c -o taint.bc
   ./build/bin/lotus-check --engine=taint taint.bc --verbose

Custom Sources and Sinks
~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   ./build/bin/lotus-check --engine=taint taint.bc \
       --taint.sources=scanf,gets,read \
       --taint.sinks=system,exec,popen

Expected Output
~~~~~~~~~~~~~~~

.. code-block:: text

   [Taint Flow Detected]
   Source: scanf (line 23)
   Sink: system (line 6) via execute_command
   Path: user_input → execute_command(user_input) → system(cmd)
   
   Warning: Unsanitized user input flows to dangerous system call

Tutorial 5: Abstract Interpretation with CLAM
----------------------------------------------

Let's use abstract interpretation to verify program properties.

Example Program
~~~~~~~~~~~~~~~

Create ``verify.c``:

.. code-block:: c

   #include <assert.h>
   
   int abs(int x) {
       if (x < 0) {
           return -x;
       }
       return x;
   }
   
   int bounded_add(int a, int b) {
       // Pre-condition: -100 <= a, b <= 100
       int result = a + b;
       // Post-condition: -200 <= result <= 200
       assert(result >= -200 && result <= 200);
       return result;
   }
   
   int div_safe(int a, int b) {
       if (b == 0) {
           return 0;
       }
       return a / b;
   }
   
   int main() {
       int x = abs(-10);
       assert(x >= 0);  // Should be proven
       
       int y = bounded_add(50, 60);
       // Assertion in bounded_add should hold
       
       int z = div_safe(100, 5);
       // No division by zero
       
       return 0;
   }

Compile and Preprocess
~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   clang -emit-llvm -c -g verify.c -o verify.bc
   ./build/bin/clam-pp --crab-lower-unsigned-icmp --crab-lower-select \
       verify.bc -o verify.prep.bc

Run Analysis with Different Domains
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**1. Interval Domain (Fast)**:

.. code-block:: bash

   ./build/bin/clam --crab-dom=int --crab-check=assert \
       --crab-print-invariants=true verify.prep.bc

**2. Zones Domain (Relational)**:

.. code-block:: bash

   ./build/bin/clam --crab-dom=zones --crab-check=assert \
       --crab-print-invariants=true verify.prep.bc

**3. Polyhedra (Most Precise)**:

.. code-block:: bash

   ./build/bin/clam --crab-dom=pk --crab-check=assert \
       --crab-print-invariants=true verify.prep.bc

Expected Output
~~~~~~~~~~~~~~~

.. code-block:: text

   Function: abs
   Entry: x ∈ [-∞, +∞]
   After branch (x < 0): x ∈ [-∞, -1]
   Return: retval ∈ [0, +∞]
   
   Function: bounded_add
   Entry: a ∈ [-100, 100], b ∈ [-100, 100]
   After add: result ∈ [-200, 200]
   Assertion PROVEN: result >= -200 && result <= 200
   
   Function: div_safe
   Entry: a ∈ [-∞, +∞], b ∈ [-∞, +∞]
   After check: b ∈ [-∞, -1] ∪ [1, +∞] (b ≠ 0)
   Division is SAFE: no division by zero

Export to JSON
~~~~~~~~~~~~~~

.. code-block:: bash

   ./build/bin/clam --crab-dom=zones --crab-check=assert \
       -ojson=verify_results.json verify.prep.bc

Tutorial 6: Program Dependence Graph Queries
---------------------------------------------

Let's use the PDG query language to analyze dependencies.

Example Program
~~~~~~~~~~~~~~~

Create ``security.c``:

.. code-block:: c

   #include <stdio.h>
   #include <string.h>
   #include <stdbool.h>
   
   bool authenticate(const char *password) {
       return strcmp(password, "secret123") == 0;
   }
   
   char *get_secret_data() {
       return "Confidential Information";
   }
   
   void log_message(const char *msg) {
       printf("LOG: %s\n", msg);
   }
   
   void send_to_network(const char *data) {
       printf("NETWORK: %s\n", data);
   }
   
   int main() {
       char password[100];
       printf("Enter password: ");
       scanf("%99s", password);
       
       if (authenticate(password)) {
           char *secret = get_secret_data();
           log_message("Access granted");
           // Secret should not flow to network without auth
           send_to_network(secret);
       } else {
           log_message("Access denied");
       }
       
       return 0;
   }

Build PDG and Query
~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   clang -emit-llvm -c -g security.c -o security.bc
   ./build/bin/lotus-ir-pdg-query -i security.bc

Interactive Queries
~~~~~~~~~~~~~~~~~~~

In the interactive mode, try these Cypher queries:

**1. View entire PDG**:

.. code-block:: cypher

   > MATCH (n) RETURN n

**2. Find returns of get_secret_data**:

.. code-block:: cypher

   > MATCH (func:FUNC_ENTRY)-[:PARAMETER_OUT]->(ret:INST_RET)
   > WHERE func.name = 'get_secret_data'
   > RETURN ret

**3. Find parameters of send_to_network**:

.. code-block:: cypher

   > MATCH (func:FUNC_ENTRY)-[:PARAMETER_IN]->(param:PARAM_FORMALIN)
   > WHERE func.name = 'send_to_network'
   > RETURN param

**4. Check information flow**:

.. code-block:: cypher

   > MATCH (secret:FUNC_ENTRY)-[:PARAMETER_OUT]->(secretRet:INST_RET)
   > WHERE secret.name = 'get_secret_data'
   > MATCH (network:FUNC_ENTRY)-[:PARAMETER_IN]->(networkParam:PARAM_FORMALIN)
   > WHERE network.name = 'send_to_network'
   > MATCH path = (secretRet)-[*]->(networkParam)
   > RETURN path

**5. Security Policy Check**:

.. code-block:: cypher

   > MATCH (secret:FUNC_ENTRY)-[:PARAMETER_OUT]->(secretRet:INST_RET)
   > WHERE secret.name = 'get_secret_data'
   > MATCH (network:FUNC_ENTRY)-[:PARAMETER_IN]->(networkParam:PARAM_FORMALIN)
   > WHERE network.name = 'send_to_network'
   > MATCH path = (secretRet)-[*]->(networkParam)
   > RETURN path

Batch Query File
~~~~~~~~~~~~~~~~

Create ``security_policy.txt``:

.. code-block:: cypher

   # Check if secret data flows to network
   MATCH (secret:FUNC_ENTRY)-[:PARAMETER_OUT]->(secretRet:INST_RET)
   WHERE secret.name = 'get_secret_data'
   MATCH (network:FUNC_ENTRY)-[:PARAMETER_IN]->(networkParam:PARAM_FORMALIN)
   WHERE network.name = 'send_to_network'
   MATCH path = (secretRet)-[*]->(networkParam)
   RETURN path
   
   # Check if authentication guards sensitive operations
   MATCH (auth:FUNC_ENTRY)-[:PARAMETER_OUT]->(authRet:INST_RET)
   WHERE auth.name = 'authenticate'
   MATCH (authRet)-[:CONTROLDEP_BR]->(check)
   MATCH (sensitiveOps:FUNC_ENTRY)-[:PARAMETER_IN]->(opParam:PARAM_FORMALIN)
   WHERE sensitiveOps.name = 'send_to_network'
   WHERE EXISTS {
     MATCH (check)-[:CONTROLDEP_BR]->(opParam)
   }
   RETURN opParam

Run batch queries:

.. code-block:: bash

   ./build/bin/lotus-ir-pdg-query -f security_policy.txt security.bc

Tutorial 7: Dynamic Validation with DynAA
------------------------------------------

Let's validate static analysis results against runtime behavior.

Example Program
~~~~~~~~~~~~~~~

Create ``dynamic.c``:

.. code-block:: c

   #include <stdio.h>
   
   void process(int *p, int *q) {
       *p = 10;
       *q = 20;
   }
   
   int main() {
       int x, y;
       process(&x, &y);
       printf("%d %d\n", x, y);
       return 0;
   }

Instrument and Run
~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   # 1. Compile to bitcode
   clang -emit-llvm -c -g dynamic.c -o dynamic.bc
   
   # 2. Instrument for dynamic analysis
   ./build/bin/dynaa-instrument dynamic.bc -o dynamic.inst.bc
   
   # 3. Compile instrumented code
   clang dynamic.inst.bc build/libRuntime.a -o dynamic.inst
   
   # 4. Run and collect traces
   mkdir logs
   LOG_DIR=logs/ ./dynamic.inst
   
   # 5. Check static analysis against traces
   ./build/bin/dynaa-check dynamic.bc logs/pts.log basic-aa

View Collected Data
~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   ./build/bin/dynaa-log-dump logs/pts.log

Expected Output
~~~~~~~~~~~~~~~

.. code-block:: text

   Dynamic Analysis Report:
   
   Total pointer operations: 142
   Unique pointers: 8
   Unique objects: 6
   
   Validation against basic-aa:
   - True positives: 45
   - False positives: 2
   - False negatives: 1
   
   False Positive: p and q reported as may-alias, but never alias at runtime

Tutorial 8: Interprocedural Analysis
-------------------------------------

Let's analyze a multi-file program.

Example Programs
~~~~~~~~~~~~~~~~

Create ``utils.c``:

.. code-block:: c

   #include <stdlib.h>
   #include "utils.h"
   
   Buffer *create_buffer(size_t size) {
       Buffer *buf = (Buffer *)malloc(sizeof(Buffer));
       if (buf) {
           buf->data = (char *)malloc(size);
           buf->size = size;
           buf->used = 0;
       }
       return buf;
   }
   
   void destroy_buffer(Buffer *buf) {
       if (buf) {
           free(buf->data);
           free(buf);
       }
   }
   
   int append_data(Buffer *buf, const char *data, size_t len) {
       if (!buf || !buf->data) return -1;
       if (buf->used + len > buf->size) return -1;
       
       memcpy(buf->data + buf->used, data, len);
       buf->used += len;
       return 0;
   }

Create ``utils.h``:

.. code-block:: c

   #ifndef UTILS_H
   #define UTILS_H
   
   #include <stddef.h>
   
   typedef struct {
       char *data;
       size_t size;
       size_t used;
   } Buffer;
   
   Buffer *create_buffer(size_t size);
   void destroy_buffer(Buffer *buf);
   int append_data(Buffer *buf, const char *data, size_t len);
   
   #endif

Create ``main.c``:

.. code-block:: c

   #include <stdio.h>
   #include "utils.h"
   
   int main() {
       Buffer *buf = create_buffer(1024);
       
       if (buf) {
           append_data(buf, "Hello", 5);
           append_data(buf, " World", 6);
           
           // Use buffer data
           printf("%.*s\n", (int)buf->used, buf->data);
           
           destroy_buffer(buf);
       }
       
       return 0;
   }

Compile and Link
~~~~~~~~~~~~~~~~

.. code-block:: bash

   # Compile each file
   clang -emit-llvm -c -g utils.c -o utils.bc
   clang -emit-llvm -c -g main.c -o main.bc
   
   # Link into single module
   llvm-link utils.bc main.bc -o program.bc

Run Interprocedural Analysis
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   # Interprocedural alias analysis
   ./build/bin/lotus-alias-aser-aa -analysis-mode=1-cfa -dump-stats program.bc
   
   # Interprocedural abstract interpretation
   ./build/bin/clam --crab-inter --crab-dom=zones \
       --crab-check=null --crab-check=bounds program.bc

The interprocedural analysis will track the buffer through function calls and verify that:

- ``create_buffer`` allocates memory correctly
- ``append_data`` checks bounds before writing
- ``destroy_buffer`` frees allocated memory
- No null pointer dereferences occur

Tutorial 9: Concurrency Analysis
---------------------------------

Let's analyze a multi-threaded program.

Example Program
~~~~~~~~~~~~~~~

Create ``concurrent.c``:

.. code-block:: c

   #include <pthread.h>
   #include <stdio.h>
   
   int shared_counter = 0;
   pthread_mutex_t lock;
   
   void *increment_thread(void *arg) {
       for (int i = 0; i < 1000; i++) {
           pthread_mutex_lock(&lock);
           shared_counter++;  // Protected
           pthread_mutex_unlock(&lock);
       }
       return NULL;
   }
   
   void *buggy_thread(void *arg) {
       for (int i = 0; i < 1000; i++) {
           shared_counter++;  // Bug: race condition
       }
       return NULL;
   }
   
   int main() {
       pthread_t t1, t2;
       pthread_mutex_init(&lock, NULL);
       
       pthread_create(&t1, NULL, increment_thread, NULL);
       pthread_create(&t2, NULL, buggy_thread, NULL);
       
       pthread_join(t1, NULL);
       pthread_join(t2, NULL);
       
       printf("Counter: %d\n", shared_counter);
       
       pthread_mutex_destroy(&lock);
       return 0;
   }

Compile and Analyze
~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   clang -emit-llvm -c -g concurrent.c -o concurrent.bc
   ./build/bin/lotus-check --engine=concur concurrent.bc --verbose

Expected Output
~~~~~~~~~~~~~~~

.. code-block:: text

   [Data Race Detected]
   Variable: shared_counter
   Thread 1: increment_thread (line 10) - PROTECTED by lock
   Thread 2: buggy_thread (line 17) - UNPROTECTED
   
   Race condition: shared_counter++ in buggy_thread not protected by mutex

Best Practices
--------------

1. **Start Simple**: Begin with fast analyses to get quick feedback

2. **Incremental Precision**: If fast analyses report issues, use more precise analyses to reduce false positives

3. **Combine Analyses**: Use multiple analyses together (e.g., alias analysis + taint analysis)

4. **Preprocess When Needed**: Use ``clam-pp`` or other preprocessors to normalize code

5. **Verify Results**: Use dynamic validation (DynAA) to confirm static analysis findings

6. **Use Configuration Files**: Create YAML configs for complex analysis workflows

7. **Visualize Results**: Generate DOT files to visualize call graphs, PDGs, and alias sets

8. **Batch Processing**: Use scripts to analyze multiple files or entire codebases

Next Steps
----------

- Explore :doc:`../tools/index` for detailed tool documentation
- Read :doc:`architecture` to understand the framework design
- See :doc:`../developer/developer_guide` to extend Lotus with custom analyses
- Check :doc:`../developer/api_reference` for programmatic usage
