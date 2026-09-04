add_subdirectory(Backends)

add_subdirectory(SymAbsAI)
add_subdirectory(Sifa)
add_subdirectory(PathProgram)
add_subdirectory(Driver)
# Frontend implementations live under lib/Verification/Frontend/.
# Today this is the Boolean-program frontend consumed by NPA.
add_subdirectory(Frontend)
add_subdirectory(Transform)
add_subdirectory(Analysis)
