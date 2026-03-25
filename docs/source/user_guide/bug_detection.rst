Bug Detection with Lotus
=========================

Lotus provides comprehensive bug detection capabilities for finding security vulnerabilities and safety issues in C/C++ programs.

Overview
--------

Lotus includes multiple bug detection tools:

1. **Kint**: Integer-related bugs (overflow, division by zero, bad shift, array bounds)
2. **Taint Analysis**: Information flow and injection vulnerabilities
3. **Concurrency Checker**: Race conditions, deadlocks, OpenMP misuse, and MPI protocol bugs

Bug Categories
--------------

Memory Safety
~~~~~~~~~~~~~

**Null Pointer Dereference**:

.. code-block:: c

   Person *p = create_person(-1);  // May return NULL
   printf("%s\n", p->name);  // Bug: potential NPD

**Use-After-Free**:

.. code-block:: c

   int *ptr = malloc(sizeof(int));
   free(ptr);
   *ptr = 42;  // Bug: use after free

**Buffer Overflow**:

.. code-block:: c

   char buffer[10];
   strcpy(buffer, user_input);  // Bug: may overflow

Integer Safety
~~~~~~~~~~~~~~

**Integer Overflow**:

.. code-block:: c

   int x = INT_MAX;
   int y = x + 1;  // Bug: signed overflow

**Division by Zero**:

.. code-block:: c

   int result = x / y;  // Bug if y == 0

**Bad Shift**:

.. code-block:: c

   int result = x << 32;  // Bug: shift amount >= width

Information Flow
~~~~~~~~~~~~~~~~

**Tainted Data Flow**:

.. code-block:: c

   char cmd[256];
   scanf("%s", cmd);  // Tainted source
   system(cmd);  // Bug: unsanitized input to sink

**Information Leakage**:

.. code-block:: c

   char *secret = getPassword();
   sendToNetwork(secret);  // Bug: confidential data leaked

Concurrency
~~~~~~~~~~~

**Data Race**:

.. code-block:: c

   int shared_counter;  // Global
   void thread_func() {
       shared_counter++;  // Bug: unprotected access
   }

**Deadlock**:

.. code-block:: c

   pthread_mutex_lock(&lock1);
   pthread_mutex_lock(&lock2);  // Bug: lock ordering issue

Tool 1: Kint - Integer and Array Analysis
------------------------------------------

Kint detects integer-related and taint-style bugs using range analysis and SMT solving.

Capabilities
~~~~~~~~~~~~

1. **Integer Overflow Detection**: Signed and unsigned overflow
2. **Division by Zero**: Detect potential divisions by zero
3. **Bad Shift**: Invalid shift amounts
4. **Array Out of Bounds**: Buffer overflows and underflows
5. **Dead Branch**: Impossible conditional branches

Basic Usage
~~~~~~~~~~~

.. code-block:: bash

   # Enable all checkers
   ./build/bin/lotus-kint -check-all program.ll
   
   # Enable specific checkers
   ./build/bin/lotus-kint -check-int-overflow -check-div-by-zero program.ll
   
   # Set timeout for slow functions
   ./build/bin/lotus-kint -check-all -function-timeout=60 program.ll

Example 1: Integer Overflow
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Vulnerable Code** (``overflow.c``):

.. code-block:: c

   #include <limits.h>
   
   int calculate_size(int count, int item_size) {
       return count * item_size;  // Potential overflow
   }
   
   int main() {
       int size = calculate_size(1000000, 1000);  // Overflows
       char *buffer = malloc(size);
       // Size is wrong due to overflow
       return 0;
   }

**Detection**:

.. code-block:: bash

   clang -emit-llvm -S -g overflow.c -o overflow.ll
   ./build/bin/lotus-kint -check-int-overflow overflow.ll

**Expected Output**:

.. code-block:: text

   [Integer Overflow] Function: calculate_size
   Location: overflow.c:4
   Instruction: %mul = mul nsw i32 %count, %item_size
   Reason: Multiplication may overflow
   Range: count ∈ [-∞, +∞], item_size ∈ [-∞, +∞]

**Fix**:

.. code-block:: c

   #include <limits.h>
   #include <stdint.h>
   
   int calculate_size(int count, int item_size) {
       // Check for overflow before multiplication
       if (count > 0 && item_size > INT_MAX / count) {
           return -1;  // Error: would overflow
       }
       return count * item_size;
   }

Example 2: Array Out of Bounds
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Vulnerable Code** (``buffer.c``):

.. code-block:: c

   void process_array(int *arr, int index, int value) {
       arr[index] = value;  // No bounds check
   }
   
   int main() {
       int buffer[10];
       int user_index;
       scanf("%d", &user_index);
       process_array(buffer, user_index, 42);  // Bug if index >= 10
       return 0;
   }

**Detection**:

.. code-block:: bash

   clang -emit-llvm -S -g buffer.c -o buffer.ll
   ./build/bin/lotus-kint -check-array-oob buffer.ll

**Expected Output**:

.. code-block:: text

   [Array Out of Bounds] Function: process_array
   Location: buffer.c:2
   Array size: 10 elements
   Index range: [-∞, +∞]
   Potential overflow: index may be >= 10

**Fix**:

.. code-block:: c

   void process_array(int *arr, int size, int index, int value) {
       if (index < 0 || index >= size) {
           return;  // Invalid index
       }
       arr[index] = value;
   }

Example 3: Division by Zero
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Vulnerable Code** (``division.c``):

.. code-block:: c

   int calculate_average(int total, int count) {
       return total / count;  // Bug if count == 0
   }
   
   int main() {
       int avg1 = calculate_average(100, 10);  // OK
       int avg2 = calculate_average(100, 0);   // Bug!
       return 0;
   }

**Detection**:

.. code-block:: bash

   clang -emit-llvm -S -g division.c -o division.ll
   ./build/bin/lotus-kint -check-div-by-zero division.ll

**Fix**:

.. code-block:: c

   int calculate_average(int total, int count) {
       if (count == 0) {
           return 0;  // Handle zero case
       }
       return total / count;
   }

Tool 2: Taint Analysis
-----------------------

Interprocedural taint analysis using the IFDS framework to track information flow.

Capabilities
~~~~~~~~~~~~

1. **Source Tracking**: Identify tainted data sources
2. **Sink Detection**: Find dangerous operations
3. **Path Finding**: Compute flows from sources to sinks
4. **Custom Sources/Sinks**: User-defined taint specifications

Basic Usage
~~~~~~~~~~~

.. code-block:: bash

   # Basic taint analysis
   ./build/bin/lotus-taint program.bc
   
   # Custom sources and sinks
   ./build/bin/lotus-taint -sources="read,scanf,recv" \
                            -sinks="system,exec,printf" \
                            program.bc
   
   # Verbose output
   ./build/bin/lotus-taint -verbose -max-results=20 program.bc

Example 1: Command Injection
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Vulnerable Code** (``cmd_injection.c``):

.. code-block:: c

   #include <stdio.h>
   #include <stdlib.h>
   #include <string.h>
   
   void execute_user_command(const char *user_input) {
       char command[256];
       sprintf(command, "cat %s", user_input);
       system(command);  // Bug: command injection
   }
   
   char *sanitize(const char *input) {
       // Remove dangerous characters
       char *output = strdup(input);
       for (int i = 0; output[i]; i++) {
           if (output[i] == ';' || output[i] == '|' || output[i] == '&') {
               output[i] = '_';
           }
       }
       return output;
   }
   
   int main() {
       char user_file[256];
       printf("Enter filename: ");
       scanf("%255s", user_file);  // Source of tainted data
       
       // Vulnerable: direct use
       execute_user_command(user_file);
       
       // Safe: sanitized
       char *safe_file = sanitize(user_file);
       execute_user_command(safe_file);
       free(safe_file);
       
       return 0;
   }

**Detection**:

.. code-block:: bash

   clang -emit-llvm -c -g cmd_injection.c -o cmd_injection.bc
   ./build/bin/lotus-taint -verbose cmd_injection.bc

**Expected Output**:

.. code-block:: text

   [Taint Flow Detected]
   Source: scanf at cmd_injection.c:24
   Sink: system at cmd_injection.c:8 (via execute_user_command)
   
   Flow path:
   1. scanf("%255s", user_file) - line 24
   2. user_file is tainted
   3. execute_user_command(user_file) - line 27
   4. sprintf(command, "cat %s", user_input) - line 7
   5. system(command) - line 8
   
   Vulnerability: User input flows to system() without sanitization

**Fix**: Always sanitize user input or use parameterized APIs:

.. code-block:: c

   void execute_user_command(const char *user_input) {
       // Validate input
       if (strchr(user_input, ';') || strchr(user_input, '|')) {
           fprintf(stderr, "Invalid filename\n");
           return;
       }
       
       // Use safe API
       execlp("cat", "cat", user_input, NULL);
   }

Example 2: SQL Injection
~~~~~~~~~~~~~~~~~~~~~~~~

**Vulnerable Code** (``sql_injection.c``):

.. code-block:: c

   void search_user(sqlite3 *db, const char *username) {
       char query[512];
       sprintf(query, "SELECT * FROM users WHERE name='%s'", username);
       sqlite3_exec(db, query, callback, 0, &err);  // Bug: SQL injection
   }
   
   int main() {
       char input[256];
       scanf("%s", input);  // Tainted
       search_user(db, input);  // Bug
       return 0;
   }

**Detection**:

.. code-block:: bash

   ./build/bin/lotus-taint -sources="scanf" -sinks="sqlite3_exec" sql_injection.bc

**Fix**: Use parameterized queries:

.. code-block:: c

   void search_user(sqlite3 *db, const char *username) {
       sqlite3_stmt *stmt;
       const char *query = "SELECT * FROM users WHERE name=?";
       sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
       sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
       sqlite3_step(stmt);
       sqlite3_finalize(stmt);
   }

Tool 3: Concurrency Checker
----------------------------

Detects concurrency bugs in multi-threaded programs.

Capabilities
~~~~~~~~~~~~

1. **Data Race Detection**: Unprotected shared memory access
2. **Deadlock Detection**: Lock ordering issues
3. **Atomicity Violations**: Non-atomic operation sequences
4. **Lock/Unlock Pairing**: Mismatched lock operations
5. **OpenMP Checks**: Taskgroup/atomic region mismatches and partial task synchronization
6. **MPI Checks**: Request lifecycle bugs, collective mismatches, blocking deadlocks, and RMA issues

Basic Usage
~~~~~~~~~~~

.. code-block:: bash

   ./build/bin/lotus-concur program.bc
   ./build/bin/lotus-concur -verbose program.bc
   ./build/bin/lotus-concur --checks=openmp,mpi program.bc

OpenMP and MPI Examples
~~~~~~~~~~~~~~~~~~~~~~~

The concurrency checker also includes dedicated runtime-aware checks for
OpenMP and MPI programs.

**OpenMP examples**:

* unmatched ``__kmpc_taskgroup`` / ``__kmpc_end_taskgroup`` pairs
* unmatched ``__kmpc_atomic_start`` / ``__kmpc_atomic_end`` pairs
* selective ``__kmpc_omp_wait_deps`` synchronization that may not fully order sibling tasks

**MPI examples**:

* ``MPI_Isend`` / ``MPI_Irecv`` requests that are never completed
* mismatched or rank-conditional collectives
* simple blocking send/recv deadlock cycles
* unsynchronized RMA operations or leaked windows

For MPI/OpenMP-heavy codebases, a useful first pass is:

.. code-block:: bash

   ./build/bin/lotus-concur --checks=openmp,mpi --report-json=parallel.json program.bc

Example: Data Race
~~~~~~~~~~~~~~~~~~

**Vulnerable Code** (``race.c``):

.. code-block:: c

   #include <pthread.h>
   
   int shared_counter = 0;
   pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
   
   void *thread1(void *arg) {
       for (int i = 0; i < 1000; i++) {
           shared_counter++;  // Bug: unprotected
       }
       return NULL;
   }
   
   void *thread2(void *arg) {
       for (int i = 0; i < 1000; i++) {
           pthread_mutex_lock(&lock);
           shared_counter++;  // Protected
           pthread_mutex_unlock(&lock);
       }
       return NULL;
   }
   
   int main() {
       pthread_t t1, t2;
       pthread_create(&t1, NULL, thread1, NULL);
       pthread_create(&t2, NULL, thread2, NULL);
       pthread_join(t1, NULL);
       pthread_join(t2, NULL);
       printf("Counter: %d\n", shared_counter);
       return 0;
   }

**Detection**:

.. code-block:: bash

   clang -emit-llvm -c -g -pthread race.c -o race.bc
   ./build/bin/lotus-concur -verbose race.bc

**Expected Output**:

.. code-block:: text

   [Data Race Detected]
   Variable: shared_counter (global)
   
   Thread 1 (thread1):
   - Location: race.c:8
   - Operation: increment (write)
   - Lock status: UNPROTECTED
   
   Thread 2 (thread2):
   - Location: race.c:16
   - Operation: increment (write)
   - Lock status: PROTECTED by lock
   
   Conflict: Both threads access shared_counter, but thread1 is unprotected

**Fix**:

.. code-block:: c

   void *thread1(void *arg) {
       for (int i = 0; i < 1000; i++) {
           pthread_mutex_lock(&lock);
           shared_counter++;  // Now protected
           pthread_mutex_unlock(&lock);
       }
       return NULL;
   }

Best Practices
--------------

1. **Start with Fast Checks**:

   .. code-block:: bash

      # Quick scan with Kint
      ./build/bin/lotus-kint -check-all program.ll

2. **Use Appropriate Checkers**:

   - Integer bugs → Kint
   - Information flow → Taint analysis
   - Concurrency → Concurrency checker

3. **Iterative Analysis**:

   - Fix obvious bugs first
   - Re-run with higher precision
   - Validate with dynamic analysis

4. **Combine with Testing**:

   - Use static analysis to find bugs
   - Write tests for found vulnerabilities
   - Verify fixes with both static and dynamic analysis

5. **Reduce False Positives**:

   - Add assertions to help analysis
   - Use more precise analyses
   - Validate with DynAA

Output Formats
--------------

Text Output
~~~~~~~~~~~

Default human-readable output:

.. code-block:: bash

   ./build/bin/lotus-kint -check-all program.ll

JSON Output
~~~~~~~~~~~

Machine-readable JSON for integration:

.. code-block:: bash

   ./build/bin/lotus-kint -check-all -output-json=results.json program.ll

SARIF Output
~~~~~~~~~~~~

Standard format for security tools:

.. code-block:: bash

   ./build/bin/lotus-kint -check-all -output-sarif=results.sarif program.ll

Integration Examples
--------------------

CI/CD Pipeline
~~~~~~~~~~~~~~

.. code-block:: bash

   #!/bin/bash
   # Build program
   clang -emit-llvm -c -g source.c -o source.bc
   
   # Run checkers
   ./lotus-kint -check-all source.ll > kint_results.txt
   ./lotus-taint source.bc > taint_results.txt
   
   # Check for bugs
   if grep -q "Bug" *.txt; then
       echo "Bugs found!"
       exit 1
   fi

VS Code Integration
~~~~~~~~~~~~~~~~~~~

Use SARIF output with VS Code SARIF viewer extension.

See Also
--------

- :doc:`tutorials` - Hands-on examples
- :doc:`troubleshooting` - Common issues
- :doc:`../tools/checker` - Detailed tool documentation
- :doc:`../developer/api_reference` - Programmatic usage
