add_library(CanaryAliasPtsSet STATIC
  BDDPtsSet.cpp
  HashConsedPointsToSet.cpp
)

target_link_libraries(CanaryAliasPtsSet
  PUBLIC CanaryCUDD
)
