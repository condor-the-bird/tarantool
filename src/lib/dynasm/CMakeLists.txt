set(lib_sources
    dynasm.c
)

set_source_files_compile_flags(${lib_sources})
add_library(dynasm STATIC ${lib_sources})
