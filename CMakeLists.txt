cmake_minimum_required(VERSION 3.28)

project(CadLens VERSION 1.0.4 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE CADLENS_SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(EXISTS "${CADLENS_SDK_PATH}/usr/include/c++/v1")
        add_compile_options(
            "SHELL:-isystem ${CADLENS_SDK_PATH}/usr/include/c++/v1"
        )
    endif()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    OpenGL
    OpenGLWidgets
    Svg
    Concurrent
    PrintSupport
)
if(APPLE
   AND NOT CMAKE_OSX_DEPLOYMENT_TARGET
   AND DEFINED QT_SUPPORTED_MIN_MACOS_VERSION)
    set(CMAKE_OSX_DEPLOYMENT_TARGET
        "${QT_SUPPORTED_MIN_MACOS_VERSION}"
        CACHE STRING "Minimum macOS version required by Qt" FORCE)
endif()
find_package(Iconv REQUIRED)

set(LIBDXFRW_BUILD_DOC OFF CACHE BOOL "" FORCE)
set(LIBDXFRW_BUILD_DWG2DXF OFF CACHE BOOL "" FORCE)
set(LIBDXFRW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBDXFRW_INSTALL_DEV OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libdxfrw EXCLUDE_FROM_ALL)

set(LIBREDWG_LIBONLY ON CACHE BOOL "" FORCE)
set(LIBREDWG_DISABLE_WRITE ON CACHE BOOL "" FORCE)
set(LIBREDWG_DISABLE_JSON ON CACHE BOOL "" FORCE)
set(DISABLE_WERROR ON CACHE BOOL "" FORCE)
set(ENABLE_LTO OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(_CADLENS_C_FLAGS_BACKUP "${CMAKE_C_FLAGS}")
set(CMAKE_C_FLAGS
    "${CMAKE_C_FLAGS} -w -D_DARWIN_C_SOURCE -D_GNU_SOURCE")
add_subdirectory(third_party/libredwg EXCLUDE_FROM_ALL)
set(CMAKE_C_FLAGS "${_CADLENS_C_FLAGS_BACKUP}")
foreach(_PRIVATE_TARGET redwg dxfrw)
    if(TARGET ${_PRIVATE_TARGET})
        set_property(TARGET ${_PRIVATE_TARGET} PROPERTY
                     C_VISIBILITY_PRESET hidden)
        set_property(TARGET ${_PRIVATE_TARGET} PROPERTY
                     CXX_VISIBILITY_PRESET hidden)
        set_property(TARGET ${_PRIVATE_TARGET} PROPERTY
                     VISIBILITY_INLINES_HIDDEN YES)
    endif()
endforeach()
if(TARGET dxfrw AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(dxfrw PRIVATE -Wno-unused-private-field)
endif()

add_library(cad_geometry STATIC
    src/core/cad_geometry.c
)
target_include_directories(cad_geometry PUBLIC src/core)

add_library(cadview_core STATIC
    src/CadModel.cpp
    src/CadModel.h
    src/LibreDwgReader.cpp
    src/LibreDwgReader.h
    src/Measurements.cpp
    src/Measurements.h
    src/SpatialIndex.cpp
    src/SpatialIndex.h
    src/SnapEngine.cpp
    src/SnapEngine.h
    src/PdfDocument.h
    $<$<PLATFORM_ID:Darwin>:src/PdfDocument.mm>
    $<$<NOT:$<PLATFORM_ID:Darwin>>:src/PdfDocument.cpp>
    third_party/libdxfrw/dwg2dxf/dx_iface.cpp
)
target_include_directories(cadview_core PRIVATE
    third_party/libdxfrw/dwg2dxf
)
if(WIN32)
    target_include_directories(cadview_core PRIVATE
        third_party/pdfium/include
    )
endif()
target_link_libraries(cadview_core
    PUBLIC
        cad_geometry
        Qt6::Core
        Qt6::Gui
    PRIVATE
        dxfrw
        redwg
        Iconv::Iconv
)
if(APPLE)
    target_link_libraries(cadview_core PRIVATE
        "-framework PDFKit"
        "-framework CoreGraphics"
    )
    set_source_files_properties(src/PdfDocument.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
endif()

add_executable(CadLens MACOSX_BUNDLE
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/FileAssociation.cpp
    src/FileAssociation.h
    src/StartScreen.cpp
    src/StartScreen.h
    src/CadView.cpp
    src/CadView.h
    resources/resources.qrc
)
target_link_libraries(CadLens PRIVATE
    cadview_core
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Svg
    Qt6::Concurrent
    Qt6::PrintSupport
)
if(APPLE)
    target_link_libraries(CadLens PRIVATE
        "-framework CoreFoundation"
        "-framework CoreServices"
    )
    target_sources(CadLens PRIVATE
        resources/macos/CadLensDWG.icns
        resources/macos/CadLensDXF.icns
    )
    set_source_files_properties(
        resources/macos/CadLensDWG.icns
        resources/macos/CadLensDXF.icns
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources
    )
endif()
if(WIN32)
    target_sources(CadLens PRIVATE resources/windows/CadLens.rc)
    target_link_libraries(CadLens PRIVATE shell32 shlwapi ole32)
    set_target_properties(CadLens PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
target_compile_options(CadLens PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
    $<$<AND:$<CXX_COMPILER_ID:AppleClang,Clang,GNU>,$<NOT:$<CONFIG:Debug>>>:-fvisibility=hidden>
    $<$<AND:$<CXX_COMPILER_ID:AppleClang,Clang,GNU>,$<NOT:$<CONFIG:Debug>>>:-fvisibility-inlines-hidden>
    $<$<AND:$<CXX_COMPILER_ID:AppleClang,Clang,GNU>,$<NOT:$<CONFIG:Debug>>>:-fstack-protector-strong>
    $<$<AND:$<CXX_COMPILER_ID:AppleClang,Clang,GNU>,$<NOT:$<CONFIG:Debug>>>:-D_FORTIFY_SOURCE=2>
    $<$<AND:$<CXX_COMPILER_ID:AppleClang,Clang,GNU>,$<NOT:$<CONFIG:Debug>>>:-DQT_NO_DEBUG_OUTPUT>
)
target_link_options(CadLens PRIVATE
    $<$<AND:$<PLATFORM_ID:Darwin>,$<CXX_COMPILER_ID:AppleClang,Clang,GNU>,$<NOT:$<CONFIG:Debug>>>:-Wl,-dead_strip>
    $<$<AND:$<PLATFORM_ID:Darwin>,$<CXX_COMPILER_ID:AppleClang,Clang>>:-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/script/CadLens.exports>
)
target_compile_definitions(CadLens PRIVATE CADLENS_VERSION="${PROJECT_VERSION}")
set_target_properties(CadLens PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
foreach(_CORE_TARGET cad_geometry cadview_core)
    set_target_properties(${_CORE_TARGET} PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
endforeach()

set_target_properties(CadLens PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "速看CAD"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.cadlens.desktop"
    MACOSX_BUNDLE_INFO_PLIST
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    OUTPUT_NAME "CadLens"
)

install(TARGETS CadLens BUNDLE DESTINATION .)

include(CheckIPOSupported)
check_ipo_supported(RESULT CADLENS_IPO_SUPPORTED
                    OUTPUT CADLENS_IPO_DIAGNOSTIC)
if(CADLENS_IPO_SUPPORTED)
    set_property(TARGET CadLens cadview_core cad_geometry PROPERTY
                 INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
else()
    message(STATUS "IPO/LTO unavailable: ${CADLENS_IPO_DIAGNOSTIC}")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(cad_geometry_tests tests/cad_geometry_tests.cpp)
    target_link_libraries(cad_geometry_tests PRIVATE cad_geometry)
    add_test(NAME cad_geometry_tests COMMAND cad_geometry_tests)

    add_executable(cadview_interaction_tests
        tests/cadview_interaction_tests.cpp
        src/CadView.cpp
        src/CadView.h
    )
    target_include_directories(cadview_interaction_tests PRIVATE
        src
        resources
    )
    target_link_libraries(cadview_interaction_tests PRIVATE
        cadview_core
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        Qt6::Svg
        Qt6::PrintSupport
    )
    add_test(NAME cadview_interaction_tests
        COMMAND cadview_interaction_tests
    )
    set_tests_properties(cadview_interaction_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(cad_document_tests tests/cad_document_tests.cpp)
    target_include_directories(cad_document_tests PRIVATE src)
    target_compile_definitions(cad_document_tests PRIVATE
        CADLENS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(cad_document_tests PRIVATE
        cadview_core
        Qt6::Core
    )
    add_test(NAME cad_document_tests COMMAND cad_document_tests)
    set_tests_properties(cad_document_tests PROPERTIES TIMEOUT 30)
endif()
