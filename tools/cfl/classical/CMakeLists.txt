add_executable(lotus-cfl-classical lotus-cfl-classical.cpp)
target_link_libraries(lotus-cfl-classical PRIVATE CanaryClassicalCFL)

add_executable(lotus-cfl-foldability lotus-cfl-foldability.cpp)
target_link_libraries(lotus-cfl-foldability PRIVATE CanaryClassicalCFL)

add_executable(lotus-cfl-pocr lotus-cfl-pocr.cpp)
target_link_libraries(lotus-cfl-pocr PRIVATE CanaryClassicalCFL)

add_executable(lotus-cfl-alias lotus-cfl-alias.cpp)
target_link_libraries(lotus-cfl-alias PRIVATE
    CanaryClassicalCFLAliasClient
    LLVMIRReader
    LLVMAsmParser
)

add_executable(lotus-cfl-vf lotus-cfl-vf.cpp)
target_link_libraries(lotus-cfl-vf PRIVATE
    CanaryClassicalCFLValueFlowClient
    LLVMIRReader
    LLVMAsmParser
)
