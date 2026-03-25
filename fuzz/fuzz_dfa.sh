#!/bin/bash
# for testing whether crashes occur in the dataflow analyses.
# export CLANG="/path/to/your/clang"
CLANG="${CLANG:-clang}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"
CSMITH="$BUILD_DIR/csmith-install/bin/csmith"
CSMITH_HOME="$BUILD_DIR/csmith-install/include"

# Get SDK path only on macOS
SDK_PATH=""
if [[ "$OSTYPE" == "darwin"* ]]; then
    SDK_PATH="$(xcrun --show-sdk-path 2>/dev/null || true)"
fi

while true; do
    C_FILE="$SCRIPT_DIR/test_$$.c"
    BC_FILE="$SCRIPT_DIR/test_$$.bc"

    # Generate random C program
    echo "=== Generating C file: $C_FILE ==="
    CSMITH_CMD="$CSMITH --pointers --structs --unions --arrays --volatile-pointers --const-pointers --jumps --embedded-assigns"
    CSMITH_CMD="$CSMITH_CMD --max-pointer-depth $((RANDOM % 3 + 1))"
    CSMITH_CMD="$CSMITH_CMD --max-struct-fields $((RANDOM % 8 + 3))"
    CSMITH_CMD="$CSMITH_CMD --max-union-fields $((RANDOM % 5 + 2))"
    CSMITH_CMD="$CSMITH_CMD --max-expr-complexity $((RANDOM % 10 + 5))"
    CSMITH_CMD="$CSMITH_CMD --max-block-depth $((RANDOM % 3 + 2))"
    CSMITH_CMD="$CSMITH_CMD --max-block-size $((RANDOM % 3 + 2))"

    if ! timeout 10s bash -c "$CSMITH_CMD > \"$C_FILE\" 2>/dev/null"; then
        echo "✗ Failed to generate C file (timeout or error)"
        continue
    fi
    echo "✓ C file generated"
    
    # Compile to LLVM IR
    echo "=== Compiling to LLVM IR: $BC_FILE ==="
    CMD="$CLANG ${SDK_PATH:+-isysroot \"$SDK_PATH\"} -I\"$CSMITH_HOME\" -w -emit-llvm -c \"$C_FILE\" -o \"$BC_FILE\""
    echo "Command: $CMD"
    if ! eval "$CMD" 2>&1; then
        echo "Output: Compilation failed"
        rm -f "$C_FILE"
        continue
    fi
    echo "Output: Compilation successful"
    
    # Run lotus-dfa-elim with different analysis options
    echo "=== Running lotus-dfa-elim ==="
    for analysis in liveness reaching_defs uninitialized constant_prop available_exprs reachable; do
        echo "--- lotus-dfa-elim with --analysis=$analysis ---"
        if ! "$BUILD_DIR/bin/lotus-dfa-elim" --analysis="$analysis" "$BC_FILE" 2>&1; then
            echo "CRASH: lotus-dfa-elim (--analysis=$analysis) crashed on $C_FILE"
            echo "Test files preserved: $C_FILE, $BC_FILE"
            exit 1
        fi
        echo "✓ lotus-dfa-elim (--analysis=$analysis) completed successfully"
    done
    
    # Run lotus-dfa-mono with different analysis options
    echo "=== Running lotus-dfa-mono ==="
    for analysis in liveness reachable constant_prop uninitialized; do
        echo "--- lotus-dfa-mono with --analysis=$analysis ---"
        if ! "$BUILD_DIR/bin/lotus-dfa-mono" --analysis="$analysis" "$BC_FILE" 2>&1; then
            echo "CRASH: lotus-dfa-mono (--analysis=$analysis) crashed on $C_FILE"
            echo "Test files preserved: $C_FILE, $BC_FILE"
            exit 1
        fi
        echo "✓ lotus-dfa-mono (--analysis=$analysis) completed successfully"
    done
    
    # Run lotus-dfa-ifds with different analysis options
    echo "=== Running lotus-dfa-ifds ==="
    for analysis in reaching_defs uninitialized; do
        echo "--- lotus-dfa-ifds with --analysis=$analysis ---"
        if ! "$BUILD_DIR/bin/lotus-dfa-ifds" --analysis="$analysis" "$BC_FILE" 2>&1; then
            echo "CRASH: lotus-dfa-ifds (--analysis=$analysis) crashed on $C_FILE"
            echo "Test files preserved: $C_FILE, $BC_FILE"
            exit 1
        fi
        echo "✓ lotus-dfa-ifds (--analysis=$analysis) completed successfully"
    done
    
    # Run lotus-dfa (unified) with different analysis options and engines
    echo "=== Running lotus-dfa ==="
    for analysis in liveness reaching_defs uninitialized constant_prop available_exprs reachable; do
        for engine in elim mono ifds; do
            echo "--- lotus-dfa with --analysis=$analysis --engine=$engine ---"
            if ! "$BUILD_DIR/bin/lotus-dfa" --analysis="$analysis" --engine="$engine" "$BC_FILE" 2>&1; then
                echo "CRASH: lotus-dfa (--analysis=$analysis --engine=$engine) crashed on $C_FILE"
                echo "Test files preserved: $C_FILE, $BC_FILE"
                exit 1
            fi
            echo "✓ lotus-dfa (--analysis=$analysis --engine=$engine) completed successfully"
        done
    done
    
    # Cleanup if no crash
    rm -f "$C_FILE" "$BC_FILE"
done
