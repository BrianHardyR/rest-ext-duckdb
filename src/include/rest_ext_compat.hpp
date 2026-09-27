// rest_ext_compat.hpp
//
// A translation-unit-independent way to detect whether DuckDB's ExtensionLoader exists
// (introduced in DuckDB v1.5.x, replacing the older Extension::Load(DuckDB&) entry point - see
// rest_ext_extension.hpp/cpp and rest_secrets.hpp/cpp).
//
// This can't be a CMake-detected `target_compile_definitions` flag like the extension's other two
// compatibility checks (see CMakeLists.txt's REST_EXT_HAS_STORAGE_EXTENSION_REGISTER and
// REST_EXT_HAS_ATTACH_OPTIONS): DuckDB v1.5.x's own build system also compiles a generated
// "generated_extension_loader.cpp" aggregator file (used to statically link every registered
// extension into the duckdb/unittest binaries) that transitively includes rest_ext_extension.hpp.
// That file belongs to a DuckDB-core CMake target, not one of ours, so it never receives our
// target's private compile definitions - if this were a CMake-set macro, that one translation unit
// would silently see it as undefined and declare the wrong Load() signature, breaking the build.
// `__has_include` is evaluated by the preprocessor the same way no matter which target/compile
// command is doing the including, so it stays correct there too.
#pragma once

#if defined(__has_include)
#if __has_include("duckdb/main/extension/extension_loader.hpp")
#define REST_EXT_HAS_EXTENSION_LOADER 1
#endif
#endif
