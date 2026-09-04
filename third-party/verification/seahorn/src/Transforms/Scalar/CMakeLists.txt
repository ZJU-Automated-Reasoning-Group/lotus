set(SEA_TRANSFORMS_SCALAR_SOURCES
  LoopUnhoist.cc
  LowerCstExpr.cc
  LowerGvInitializers.cc
  LowerArithInstrinsics.cc
  LowerLibCxxAbiFunctions.cc
  PromoteVerifierCalls.cc
  CutLoops.cc
  PromoteMalloc.cc
  KillVarArgFn.cc
  PromoteBoolLoads.cc
  SimplifyPointerLoops.cc
  SymbolizeConstantLoopBounds.cc
  LowerAssert.cc
  UnfoldLoopForDsa.cc
  PromoteSeahornAssume.cc
  PromoteMemcpy.cc
  UnifyAssumes.cc
  LoopPeeler.cc
  BackedgeCutter.cc
  LowerIsDeref.cc
)

if(HAVE_CLAM)
  list(APPEND SEA_TRANSFORMS_SCALAR_SOURCES CrabLowerIsDeref.cc)
endif()

add_library(SeaTransformsScalar STATIC ${SEA_TRANSFORMS_SCALAR_SOURCES})

target_link_libraries(SeaTransformsScalar PUBLIC
  CanaryTransform
)

if(HAVE_CLAM)
  target_link_libraries(SeaTransformsScalar PRIVATE ${CLAM_LIBS})
endif()
