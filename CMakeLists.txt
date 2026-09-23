# Recursively collect all module sources
file(GLOB_RECURSE SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp" "${CMAKE_CURRENT_LIST_DIR}/src/*.h")

foreach(source ${SOURCES})
    AC_ADD_SCRIPT("${source}")
endforeach()

# Register the loader header with the core
AC_ADD_SCRIPT_LOADER("mod_guildhouse" "${CMAKE_CURRENT_LIST_DIR}/src/loader.cpp")
