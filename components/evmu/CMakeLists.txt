# libEVMU (Falco Girgis) and libGimbal, built with their own CMake projects.
idf_component_register(SRCS "evmu_port.c" REQUIRES pthread)

add_subdirectory(libevmu ${CMAKE_CURRENT_BINARY_DIR}/libevmu EXCLUDE_FROM_ALL)

target_link_libraries(${COMPONENT_LIB} PUBLIC libLibElysianVMU)
# libGimbal needs the port shims; link them after it.
target_link_libraries(libGimbal ${COMPONENT_LIB})

# Upstream code has warnings that IDF's -Werror would turn into build failures.
target_compile_options(libLibElysianVMU PRIVATE -Wno-error)

# Release-style build: compile out the asserts in the emulator's hot paths.
target_compile_definitions(libLibElysianVMU PRIVATE NDEBUG)
target_compile_definitions(libGimbal PRIVATE NDEBUG)
