cmake_minimum_required(VERSION 3.22)
project(cadcore LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(LIBREDWG_DIR ../../../../../third_party/libredwg)
get_filename_component(LIBREDWG_DIR "${LIBREDWG_DIR}" ABSOLUTE)
set(ROOT_DIR ../../../../..)
get_filename_component(ROOT_DIR "${ROOT_DIR}" ABSOLUTE)
set(CAD_CORE_DIR "${ROOT_DIR}/src/core")

set(_SAVED_C_FLAGS "${CMAKE_C_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w -D_GNU_SOURCE")
add_subdirectory(${LIBREDWG_DIR} libredwg-build EXCLUDE_FROM_ALL)
set(CMAKE_C_FLAGS "${_SAVED_C_FLAGS}")

add_library(cadcore SHARED
    cadcore.cpp
    corebridge.cpp
    "${CAD_CORE_DIR}/cad_geometry.c"
    rarbridge.cpp
)

# 官方 unrar 源码（RAR4/RAR5），以 RARDLL 模式静态编译。
# 注意：unrar 的不少 .cpp 是被其它文件 #include 的片段，
# 必须按官方 makefile 的 OBJECTS+LIB_OBJ 精确列表编译。
set(UNRAR_SOURCES
    unrar/rar.cpp unrar/strlist.cpp unrar/strfn.cpp unrar/pathfn.cpp
    unrar/smallfn.cpp unrar/global.cpp unrar/file.cpp unrar/filefn.cpp
    unrar/filcreat.cpp unrar/archive.cpp unrar/arcread.cpp unrar/unicode.cpp
    unrar/system.cpp unrar/crypt.cpp unrar/crc.cpp unrar/rawread.cpp
    unrar/encname.cpp unrar/resource.cpp unrar/match.cpp unrar/timefn.cpp
    unrar/rdwrfn.cpp unrar/consio.cpp unrar/options.cpp unrar/errhnd.cpp
    unrar/rarvm.cpp unrar/secpassword.cpp unrar/rijndael.cpp unrar/getbits.cpp
    unrar/sha1.cpp unrar/sha256.cpp unrar/blake2s.cpp unrar/hash.cpp
    unrar/extinfo.cpp unrar/extract.cpp unrar/volume.cpp unrar/list.cpp
    unrar/find.cpp unrar/unpack.cpp unrar/headers.cpp unrar/threadpool.cpp
    unrar/rs16.cpp unrar/cmddata.cpp unrar/ui.cpp unrar/largepage.cpp
    unrar/filestr.cpp unrar/scantree.cpp unrar/dll.cpp unrar/qopen.cpp
)
add_library(unrar STATIC ${UNRAR_SOURCES})
target_compile_definitions(unrar PRIVATE RARDLL SILENT _FILE_OFFSET_BITS=64 _LARGEFILE64_SOURCE)
target_compile_options(unrar PRIVATE -O2 -w)
target_include_directories(unrar PRIVATE unrar)

target_include_directories(cadcore PRIVATE
    ${LIBREDWG_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/libredwg-build/include
    ${CAD_CORE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(cadcore PRIVATE redwg unrar log)
# Android 15+ 16KB 内存页对齐要求
target_link_options(cadcore PRIVATE "-Wl,-z,max-page-size=16384")
target_compile_options(cadcore PRIVATE -O2 -w)
