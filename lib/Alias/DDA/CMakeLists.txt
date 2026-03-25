# Demand-driven pointer analysis using SVFG (FlowDDA, ContextDDA, DDAClient)
# Requires: ICFG, SVFG (which uses AserPTA)

add_library(DDA STATIC
  ContextDDA.cpp
  CxtDPItem.cpp
  DDAStat.cpp
  DDAClient.cpp
  DDAPass.cpp
  FlowDDA.cpp
)

target_link_libraries(DDA PUBLIC
  SVFG
  LLVMCore
  LLVMSupport
)
