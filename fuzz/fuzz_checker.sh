#!/bin/bash
# Fuzz the checkers in tools/checker (lotus-kint, lotus-taint, lotus-concur, lotus-pulse, lotus-fitx).
# Uses CSmith to generate random C, compiles to LLVM IR, then runs each checker.
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

    # lotus-kint
    echo "=== Running lotus-kint ==="
    if ! "$BUILD_DIR/bin/lotus-kint" "$BC_FILE" 2>&1; then
        echo "CRASH: lotus-kint crashed on $C_FILE"
        echo "Test files preserved: $C_FILE, $BC_FILE"
        exit 1
    fi
    echo "✓ lotus-kint completed successfully"

    # lotus-taint (with default and a couple of aa types)
    for aa in dyck andersen; do
        echo "=== Running lotus-taint --aa=$aa ==="
        if ! "$BUILD_DIR/bin/lotus-taint" --aa="$aa" "$BC_FILE" 2>&1; then
            echo "CRASH: lotus-taint (--aa=$aa) crashed on $C_FILE"
            echo "Test files preserved: $C_FILE, $BC_FILE"
            exit 1
        fi
        echo "✓ lotus-taint (--aa=$aa) completed successfully"
    done

    # lotus-concur (all checks enabled by default; also run analysis-only)
    echo "=== Running lotus-concur ==="
    if ! "$BUILD_DIR/bin/lotus-concur" "$BC_FILE" 2>&1; then
        echo "CRASH: lotus-concur crashed on $C_FILE"
        echo "Test files preserved: $C_FILE, $BC_FILE"
        exit 1
    fi
    echo "✓ lotus-concur completed successfully"
    echo "=== Running lotus-concur --analysis-only ==="
    if ! "$BUILD_DIR/bin/lotus-concur" --analysis-only "$BC_FILE" 2>&1; then
        echo "CRASH: lotus-concur (--analysis-only) crashed on $C_FILE"
        echo "Test files preserved: $C_FILE, $BC_FILE"
        exit 1
    fi
    echo "✓ lotus-concur (--analysis-only) completed successfully"

    # lotus-pulse (with and without SMT for coverage)
    for no_smt in false true; do
        echo "=== Running lotus-pulse --no-smt=$no_smt ==="
        if ! "$BUILD_DIR/bin/lotus-pulse" --no-smt="$no_smt" "$BC_FILE" 2>&1; then
            echo "CRASH: lotus-pulse (--no-smt=$no_smt) crashed on $C_FILE"
            echo "Test files preserved: $C_FILE, $BC_FILE"
            exit 1
        fi
        echo "✓ lotus-pulse (--no-smt=$no_smt) completed successfully"
    done

    # lotus-fitx
    echo "=== Running lotus-fitx ==="
    if ! "$BUILD_DIR/bin/lotus-fitx" "$BC_FILE" 2>&1; then
        echo "CRASH: lotus-fitx crashed on $C_FILE"
        echo "Test files preserved: $C_FILE, $BC_FILE"
        exit 1
    fi
    echo "✓ lotus-fitx completed successfully"

    # Cleanup if no crash
    rm -f "$C_FILE" "$BC_FILE"
done
