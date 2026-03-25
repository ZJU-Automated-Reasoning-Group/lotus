add_library(Annotation STATIC
    ExternalModRefTable.cpp
    ExternalModRefTablePrinter.cpp
    ExternalPointerTable.cpp
    ExternalPointerTablePrinter.cpp
    TaintConfigParser.cpp
    TaintConfigManager.cpp
    APISpec.cpp
)

# Create alias for backward compatibility
add_library(LotusAnnotation ALIAS Annotation)
