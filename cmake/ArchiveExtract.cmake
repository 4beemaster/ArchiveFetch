include_guard(GLOBAL)
include(FetchContent)
enable_language(C)
get_filename_component(ARCHIVEFETCH_EXTRACT_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Scoped dependency discovery prevents an installed DLL or an Android host library
# from silently replacing one of the pinned static compression libraries.
function(archivefetch_build_extraction)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(BUILD_TESTING OFF)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(extract_zlib URL https://zlib.net/fossils/zlib-1.3.1.tar.gz
        URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)
    FetchContent_MakeAvailable(extract_zlib)
    set_target_properties(zlib PROPERTIES EXCLUDE_FROM_ALL TRUE)
    # Other application libraries may import a different system zlib.
    target_compile_definitions(zlibstatic PUBLIC Z_PREFIX)

    FetchContent_Declare(extract_bzip2 URL https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
        URL_HASH SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269)
    FetchContent_MakeAvailable(extract_bzip2)
    set(bzip_sources blocksort.c huffman.c crctable.c randtable.c compress.c decompress.c bzlib.c)
    list(TRANSFORM bzip_sources PREPEND "${extract_bzip2_SOURCE_DIR}/")
    add_library(archivefetch_bzip2 STATIC ${bzip_sources})
    target_include_directories(archivefetch_bzip2 PUBLIC "${extract_bzip2_SOURCE_DIR}")

    foreach(tool XZ XZDEC LZMADEC LZMAINFO SCRIPTS SYMLINKS)
        set(XZ_TOOL_${tool} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(XZ_NLS OFF CACHE BOOL "" FORCE)
    set(XZ_DOC OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(extract_xz URL https://github.com/tukaani-project/xz/releases/download/v5.8.1/xz-5.8.1.tar.xz
        URL_HASH SHA256=0b54f79df85912504de0b14aec7971e3f964491af1812d83447005807513cd9e)
    FetchContent_MakeAvailable(extract_xz)
    target_include_directories(liblzma INTERFACE "$<BUILD_INTERFACE:${extract_xz_SOURCE_DIR}/src/liblzma/api>")

    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(extract_zstd
        URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
        URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
        SOURCE_SUBDIR build/cmake)
    FetchContent_MakeAvailable(extract_zstd)

    include("${ARCHIVEFETCH_EXTRACT_SOURCE_ROOT}/cmake/ArchiveCompression.cmake")
    foreach(feature TAR CPIO CAT UNZIP TEST INSTALL ACL XATTR ICONV LIBXML2 EXPAT WIN32_XMLLITE
        PCREPOSIX PCRE2POSIX OPENSSL MBEDTLS NETTLE LIBB2 LZ4 LZO ZSTD WERROR)
        set(ENABLE_${feature} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_Declare(extract_archive URL https://www.libarchive.org/downloads/libarchive-3.8.2.tar.xz
        URL_HASH SHA256=db0dee91561cbd957689036a3a71281efefd131d35d1d98ebbc32720e4da58e2)
    FetchContent_MakeAvailable(extract_archive)
    archivefetch_link_compression(archive_static)
    target_compile_definitions(archive_static PUBLIC LIBARCHIVE_STATIC)
    target_include_directories(archive_static PUBLIC "${extract_archive_SOURCE_DIR}/libarchive")

    add_library(archivefetch_extraction STATIC "${ARCHIVEFETCH_EXTRACT_SOURCE_ROOT}/ps5/extract/extract_engine.cpp")
    target_compile_features(archivefetch_extraction PUBLIC cxx_std_17)
    target_include_directories(archivefetch_extraction PUBLIC
        "${ARCHIVEFETCH_EXTRACT_SOURCE_ROOT}/ps5/extract" "${ARCHIVEFETCH_EXTRACT_SOURCE_ROOT}/src")
    target_link_libraries(archivefetch_extraction PUBLIC archive_static)
    if(WIN32)
        target_compile_definitions(archivefetch_extraction PRIVATE NOMINMAX)
    endif()
    set(notices "${CMAKE_BINARY_DIR}/extraction-licenses")
    file(MAKE_DIRECTORY "${notices}")
    configure_file("${extract_archive_SOURCE_DIR}/COPYING" "${notices}/libarchive-LICENSE.txt" COPYONLY)
    configure_file("${extract_zlib_SOURCE_DIR}/README" "${notices}/zlib-LICENSE.txt" COPYONLY)
    configure_file("${extract_bzip2_SOURCE_DIR}/LICENSE" "${notices}/bzip2-LICENSE.txt" COPYONLY)
    configure_file("${extract_xz_SOURCE_DIR}/COPYING" "${notices}/xz-LICENSE.txt" COPYONLY)
    configure_file("${extract_xz_SOURCE_DIR}/COPYING.0BSD" "${notices}/xz-0BSD.txt" COPYONLY)
    configure_file("${extract_zstd_SOURCE_DIR}/LICENSE" "${notices}/zstd-LICENSE.txt" COPYONLY)
    set(ARCHIVEFETCH_EXTRACTION_LICENSE_DIR "${notices}" PARENT_SCOPE)
    set(ARCHIVEFETCH_EXTRACTION_FIXTURE_DIR "${extract_archive_SOURCE_DIR}/libarchive/test" PARENT_SCOPE)
endfunction()

archivefetch_build_extraction()
