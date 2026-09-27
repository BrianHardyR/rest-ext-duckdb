cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME rest_ext)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then
# used in cmake with find_package. Feel free to remove or replace with other dependencies.
# Note that it should also be removed from vcpkg.json to prevent needlessly installing it..
find_package(OpenSSL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# rest_ext targets a range of DuckDB core versions (see USAGE.md), not just the one pinned by the
# ./duckdb submodule - StorageExtension registration changed between versions:
#   - DuckDB <= v1.4.x: DBConfig::storage_extensions is a plain public map of unique_ptr.
#   - DuckDB >= v1.5.x: that map became private; StorageExtension::Register(shared_ptr) is the only
#     supported way in.
# Detect which is available instead of hardcoding one version's API. The check uses `sizeof(&...)`
# rather than actually calling/linking it, so it only needs the declaration to exist - not a link
# against a compiled DuckDB library.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/src/include
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/fsst
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/fmt/include
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/hyperloglog
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/fastpforlib
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/skiplist
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/ska_sort
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/fast_float
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/re2
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/miniz
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/utf8proc/include
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/concurrentqueue
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/pcg
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/pdqsort
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/tdigest
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/mbedtls/include
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/jaro_winkler
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/vergesort
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/yyjson/include
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/zstd/include
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/libpg_query/include)
set(CMAKE_REQUIRED_DEFINITIONS -DDUCKDB_BUILD_LIBRARY)
set(CMAKE_REQUIRED_FLAGS "-std=c++17")
check_cxx_source_compiles("
    #include \"duckdb/storage/storage_extension.hpp\"
    #include \"duckdb/main/config.hpp\"
    int main() {
        (void)sizeof(&duckdb::StorageExtension::Register);
        return 0;
    }
" REST_EXT_HAS_STORAGE_EXTENSION_REGISTER)

# (The extension entry point also changed shape around the same DuckDB version - Extension::Load(DuckDB&)
# plus free ExtensionUtil::Register*() helpers, vs. Extension::Load(ExtensionLoader&) plus
# ExtensionLoader instance methods. That one is detected in src/include/rest_ext_compat.hpp instead
# of here, via __has_include rather than a CMake compile-definition: DuckDB v1.5.x's own build also
# compiles a generated "generated_extension_loader.cpp" aggregator - a DuckDB-core CMake target, not
# one of ours - that transitively includes rest_ext_extension.hpp to statically link every
# registered extension into the duckdb/unittest binaries. A macro set via THIS file's
# target_compile_definitions would never reach that separate target, silently making it see the
# wrong Load() signature; __has_include is evaluated identically no matter which target compiles a
# given translation unit, so it stays correct there too. See rest_ext_compat.hpp for the full story.)

# The attach callback itself changed shape a version earlier still:
#   - DuckDB <= v1.3.x: attach_function_t takes AttachInfo& (whose own .options map holds the
#     ATTACH options) plus a separate AccessMode.
#   - DuckDB >= v1.4.x: attach_function_t instead takes AttachInfo& plus a separate AttachOptions&
#     (which holds the options map itself).
# NOTE: an `AttachOptions` struct exists in BOTH eras (it's used elsewhere in DuckDB's own ATTACH
# handling even in v1.3.x) - merely checking the type's existence is a false positive. This checks
# what actually matters: can a function with the NEW signature be assigned to attach_function_t.
check_cxx_source_compiles("
    #include \"duckdb/storage/storage_extension.hpp\"
    #include \"duckdb/main/attached_database.hpp\"
    namespace duckdb {
    static unique_ptr<Catalog> RestExtCompatCheckAttach(optional_ptr<StorageExtensionInfo>, ClientContext &,
                                                        AttachedDatabase &, const string &, AttachInfo &,
                                                        AttachOptions &) {
        return nullptr;
    }
    }
    int main() {
        duckdb::attach_function_t fn = duckdb::RestExtCompatCheckAttach;
        (void)fn;
        return 0;
    }
" REST_EXT_HAS_ATTACH_OPTIONS)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_DEFINITIONS)
unset(CMAKE_REQUIRED_FLAGS)

set(EXTENSION_SOURCES
    src/rest_ext_extension.cpp
    src/rest_attach.cpp
    src/rest_attachment_catalog.cpp
    src/rest_namespace_catalog.cpp
    src/rest_fetch_function.cpp
    src/rest_secrets.cpp
    src/spec_import.cpp
    src/http_client.cpp
    src/json_helpers.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link OpenSSL in both the static library as the loadable extension
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)

# http_client.cpp includes DuckDB's vendored, header-only httplib directly (see its own header
# comment) - some DuckDB versions expose this path to extensions automatically via their own build
# macros and some don't, so add it explicitly rather than relying on that.
target_include_directories(${EXTENSION_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib)
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib)

if(REST_EXT_HAS_STORAGE_EXTENSION_REGISTER)
  target_compile_definitions(${EXTENSION_NAME} PRIVATE REST_EXT_HAS_STORAGE_EXTENSION_REGISTER)
  target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE REST_EXT_HAS_STORAGE_EXTENSION_REGISTER)
endif()

if(REST_EXT_HAS_ATTACH_OPTIONS)
  target_compile_definitions(${EXTENSION_NAME} PRIVATE REST_EXT_HAS_ATTACH_OPTIONS)
  target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE REST_EXT_HAS_ATTACH_OPTIONS)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
