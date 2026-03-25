
add_library(CanaryTransform STATIC
        LowerConstantExpr.cpp
        LowerGlobalConstantArraySelect.cpp
        LowerSelect.cpp
        MergeReturn.cpp
        NameBlock.cpp
        RemoveDeadBlock.cpp
        RemoveNoRetFunction.cpp
        SimplifyLatch.cpp
        # FlattenInit.cpp
        UnrollVectors.cpp
        ElimPhi.cpp
        MergeGEP.cpp
        SimplifyExtractValue.cpp
        SimplifyInsertValue.cpp
        CastElimPass.cpp
)

add_subdirectory(Nisse)
