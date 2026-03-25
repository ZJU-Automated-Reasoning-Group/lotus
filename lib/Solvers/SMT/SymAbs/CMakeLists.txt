# SymAbs: SMT Formula Abstraction Library
# 
# This library provides formula-level abstraction algorithms for SMT bit-vector formulas.
# It converts bit-vectors to linear integer formulas for approximation.
# 
# ⚠️ This is NOT the same as SymAbsAIAnalysis (lib/Verification/SymAbsAI),
# which is a program-level analysis framework for LLVM IR.
# 
# See README.md for details on the distinction.

add_library(SymAbs STATIC
        SymAbsUtils.cpp
        LinearExpression.cpp
        Octagon.cpp
        Zone.cpp
        Congruence.cpp
        Affine.cpp
        Polyhedron.cpp
        Polynomial.cpp
        SMTLibOptimizer.cpp
        SMTOptimization.cpp
)

#target_link_libraries(SymAbs
#        CanarySMT
#)
