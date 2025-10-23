cmake_minimum_required(VERSION 3.13)

project("musl-libc"
    VERSION 1.0
    LANGUAGES C ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "^aarch64.*|^armv8.*")
    set(ARCH "aarch64")
elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
    set(ARCH "x86_64")
else()
    message(FATAL_ERROR "Architecture not supported: ${CMAKE_SYSTEM_PROCESSOR}")
endif()
message(STATUS "Building for architecture: ${ARCH}")

include(CheckCCompilerFlag)
check_c_compiler_flag(-femulated-tls HAVE_EMULATED_TLS)
if(NOT HAVE_EMULATED_TLS)
    message(FATAL_ERROR "Compiler must be clang for -femulated-tls")
endif()

include(CheckLinkerFlag)
check_linker_flag(C "-static;-lgcc" USE_LIBGCC)
if(USE_LIBGCC)
    set(LIBGCC -lgcc)
else()
    set(LIBGCC -lcompiler_rt)
endif()

check_linker_flag(CXX "-static;-lgcc_eh" USE_LIBGCC_UNWIND)
if(USE_LIBGCC_UNWIND)
    set(LIBUNWIND -lgcc_eh)
else()
    set(LIBUNWIND -lunwind)
endif()

set(CFLAGS_AUTO -O2 -fno-align-functions -pipe -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -w -Wno-pointer-to-int-cast -Werror=implicit-function-declaration -Werror=implicit-int -Werror=pointer-sign -Werror=pointer-arith -Werror=int-conversion -Werror=incompatible-pointer-types -Qunused-arguments -Waddress -Warray-bounds -Wchar-subscripts -Wduplicate-decl-specifier -Winit-self -Wreturn-type -Wsequence-point -Wstrict-aliasing -Wunused-function -Wunused-label -Wunused-variable)
set(CFLAGS_C99FSE -std=c99 -nostdinc -ffreestanding -frounding-math -fno-strict-aliasing -Wa,--noexecstack)
set(CFLAGS_MEMOPS "")
set(CFLAGS_NOSSP -fno-stack-protector)

set(CFLAGS_ALL ${CFLAGS_C99FSE} -D_XOPEN_SOURCE=700 ${CFLAGS_AUTO} -fPIE)

set(GENERATED_H_DIR ${CMAKE_BINARY_DIR}/include)
set(GENERATED_INTERNAL_H_DIR ${CMAKE_BINARY_DIR}/src/internal)

set(GENERATED_ALLTYPES_H ${GENERATED_H_DIR}/bits/alltypes.h)
set(GENERATED_SYSCALL_H ${GENERATED_H_DIR}/bits/syscall.h)
set(GENERATED_VERSION_H ${GENERATED_INTERNAL_H_DIR}/version.h)

set(INCLUDE_ALL
    # Source directories
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/${ARCH}
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/generic
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    # Build directories for generated headers
    ${GENERATED_H_DIR}
    ${GENERATED_INTERNAL_H_DIR}
)

# Command to generate include/bits/alltypes.h
add_custom_command(
    OUTPUT ${GENERATED_ALLTYPES_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GENERATED_H_DIR}/bits
    COMMAND sed -f ${CMAKE_CURRENT_SOURCE_DIR}/tools/mkalltypes.sed ${CMAKE_CURRENT_SOURCE_DIR}/arch/${ARCH}/bits/alltypes.h.in ${CMAKE_CURRENT_SOURCE_DIR}/include/alltypes.h.in > ${GENERATED_ALLTYPES_H}
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/mkalltypes.sed
        ${CMAKE_CURRENT_SOURCE_DIR}/arch/${ARCH}/bits/alltypes.h.in
        ${CMAKE_CURRENT_SOURCE_DIR}/include/alltypes.h.in
    VERBATIM
    COMMENT "Generating ${GENERATED_ALLTYPES_H}"
)

# Command to generate include/bits/syscall.h
add_custom_command(
    OUTPUT ${GENERATED_SYSCALL_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GENERATED_H_DIR}/bits
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/arch/${ARCH}/bits/syscall.h.in ${GENERATED_SYSCALL_H}
    COMMAND sed -n -e "s/__NR_/SYS_/p" < ${CMAKE_CURRENT_SOURCE_DIR}/arch/${ARCH}/bits/syscall.h.in >> ${GENERATED_SYSCALL_H}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/arch/${ARCH}/bits/syscall.h.in
    VERBATIM
    COMMENT "Generating ${GENERATED_SYSCALL_H}"
)

# Command to generate version.h by running the version.sh script
execute_process(
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tools/version.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE MUSL_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
file(WRITE ${GENERATED_VERSION_H} "#define VERSION \"${MUSL_VERSION}\"\n")
# We create a custom command to ensure the file is "generated" during build time
# and dependencies are tracked, even though we already created it.
add_custom_command(
    OUTPUT ${GENERATED_VERSION_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GENERATED_INTERNAL_H_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/VERSION ${CMAKE_BINARY_DIR}/VERSION.tmp # A proxy dependency
    COMMAND sh -c "printf '#define VERSION \"%s\"\\n' \"$(cat ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)\" > ${GENERATED_VERSION_H}"
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION
    VERBATIM
    COMMENT "Generating ${GENERATED_VERSION_H}"
)

add_custom_target(generate_headers
    DEPENDS
        ${GENERATED_ALLTYPES_H}
        ${GENERATED_SYSCALL_H}
        ${GENERATED_VERSION_H}
)

function(musl_find_sources OUT_VAR SRC_DIRS_IN)
    set(BASE_SRCS "")
    foreach(dir ${SRC_DIRS_IN})
        file(GLOB dir_srcs RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "${dir}/*.c")
        list(APPEND BASE_SRCS ${dir_srcs})
    endforeach()

    set(ARCH_SRCS "")
    foreach(dir ${SRC_DIRS_IN})
        file(GLOB dir_srcs RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "${dir}/${ARCH}/*.[csS]")
        list(APPEND ARCH_SRCS ${dir_srcs})
    endforeach()

    set(SOURCES_TO_EXCLUDE "")
    foreach(arch_src ${ARCH_SRCS})
        get_filename_component(arch_dir ${arch_src} DIRECTORY)
        get_filename_component(arch_parent_dir ${arch_dir} DIRECTORY)
        get_filename_component(filename_we ${arch_src} NAME_WE)
        set(base_src_to_exclude "${arch_parent_dir}/${filename_we}.c")
        list(APPEND SOURCES_TO_EXCLUDE ${base_src_to_exclude})
    endforeach()

    if(SOURCES_TO_EXCLUDE)
        list(REMOVE_ITEM BASE_SRCS ${SOURCES_TO_EXCLUDE})
    endif()

    set(${OUT_VAR} ${BASE_SRCS} ${ARCH_SRCS} PARENT_SCOPE)
endfunction()

# -----------------------------------------------------------------------------
# CRT Object Library (crt*.o)
# -----------------------------------------------------------------------------
musl_find_sources(CRT_SOURCES_ALL "crt")
set(CRT_SOURCES ${CRT_SOURCES_ALL})
set(CRT_SOURCES_PIE ${CRT_SOURCES_ALL})

list(FILTER CRT_SOURCES INCLUDE REGEX ".*/(crt1|crti|crtn)\\.[csS]$")
list(FILTER CRT_SOURCES_PIE INCLUDE REGEX ".*/(rcrt1|crti|crtn)\\.[csS]$")

add_library(crt OBJECT ${CRT_SOURCES})
add_dependencies(crt generate_headers)
target_include_directories(crt PRIVATE ${INCLUDE_ALL})
target_compile_options(crt PRIVATE ${CFLAGS_ALL} -DCRT ${CFLAGS_NOSSP})

add_library(crt_pie OBJECT ${CRT_SOURCES_PIE})
add_dependencies(crt_pie generate_headers)
target_include_directories(crt_pie PRIVATE ${INCLUDE_ALL})
target_compile_options(crt_pie PRIVATE ${CFLAGS_ALL} -DCRT ${CFLAGS_NOSSP})

# Apply -fPIC to specific CRT files
set(CRT_SOURES_TMP ${CRT_SOURCES_ALL})
list(FILTER CRT_SOURCES_TMP INCLUDE REGEX ".*/(Scrt1|rcrt1)\\.[csS]$")
if(CRT_SOURCES_TMP)
    set_source_files_properties(${CRT_SOURCES} PROPERTIES COMPILE_FLAGS "-fPIC")
endif()

# -----------------------------------------------------------------------------
# Static Library Target (libc.a)
# -----------------------------------------------------------------------------
set(MALLOC_DIR "mallocng")
file(GLOB LIBC_SRC_SUBDIRS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "src/*")
list(APPEND LIBC_SRC_SUBDIRS "src/malloc/${MALLOC_DIR}")

musl_find_sources(LIBC_SOURCES "${LIBC_SRC_SUBDIRS}")

add_library(c STATIC ${LIBC_SOURCES})
add_dependencies(c generate_headers)
target_include_directories(c PRIVATE ${INCLUDE_ALL})
target_compile_options(c PRIVATE ${CFLAGS_ALL})
set_target_properties(c PROPERTIES LINK_INTERFACE_MULTIPLICITY 3)

# --- Per-file compile flags ---

# Apply -O3 to performance-critical files
file(GLOB OPTIMIZE_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/malloc/${MALLOC_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/string/*.c"
)
set_source_files_properties(${OPTIMIZE_SRCS} PROPERTIES COMPILE_FLAGS "-O3")

# Apply CFLAGS_MEMOPS to memory operation files
set(MEMOPS_NAMES "memcpy" "memmove" "memcmp" "memset")
set(MEMOPS_SRCS "")
foreach(name ${MEMOPS_NAMES})
    set(MEMOPS_SRCS_TMP ${LIBC_SOURCES})
    list(FILTER MEMOPS_SRCS_TMP INCLUDE REGEX ".*/${name}\\.[csS]$")
    list(APPEND MEMOPS_SRCS ${MEMOPS_SRCS_TMP})
endforeach()
if(MEMOPS_SRCS AND CFLAGS_MEMOPS)
    set_source_files_properties(${MEMOPS_SRCS} PROPERTIES COMPILE_FLAGS ${CFLAGS_MEMOPS})
endif()

# Apply CFLAGS_NOSSP (no stack protector) to specific low-level files
set(NOSSP_NAMES "__libc_start_main" "__init_tls" "__stack_chk_fail" "__set_thread_area" "memset" "memcpy")
set(NOSSP_SRCS "")
foreach(name ${NOSSP_NAMES})
    set(NOSSP_SRCS_TMP ${LIBC_SOURCES})
    list(FILTER NOSSP_SRCS_TMP INCLUDE REGEX ".*/${name}\\.[csS]$")
    list(APPEND NOSSP_SRCS ${NOSSP_SRCS_TMP})
endforeach()
if(NOSSP_SRCS AND CFLAGS_NOSSP)
    set_source_files_properties(${NOSSP_SRCS} PROPERTIES COMPILE_FLAGS ${CFLAGS_NOSSP})
endif()

# -----------------------------------------------------------------------------
# Interface for Consumers
# -----------------------------------------------------------------------------
add_library(libc INTERFACE)
target_compile_options(libc INTERFACE -nostdinc -fPIE -femulated-tls)
target_include_directories(libc INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/${ARCH}
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/generic
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${GENERATED_H_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
)

# Pass libraries as linker flags to prevent cmake from touching the order
target_link_libraries(libc INTERFACE
    -nostartfiles -nodefaultlibs -nostdlib
    -Wl,-z,muldefs
    -L${CMAKE_CURRENT_BINARY_DIR}
    -Wl,-lc
    -Wl,${LIBGCC}
    -Wl,${LIBUNWIND}
    c
)

message(STATUS "Configuration complete.")
