add_executable(lotus-cfl-classical lotus-cfl-classical.cpp)
target_link_libraries(lotus-cfl-classical PRIVATE CanaryClassicalCFL)

add_executable(lotus-cfl-alias lotus-cfl-alias.cpp)
target_link_libraries(lotus-cfl-alias PRIVATE
    CanaryClassicalCFLAlias
    LLVMIRReader
    LLVMAsmParser
)
