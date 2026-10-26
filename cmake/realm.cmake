# Realm .NET 20.1.0 (used by the inspected lazer build) uses Core 20.1.2.
# Only the storage engine is linked. No CLR, sync service, SDK or game process.
include(FetchContent)
set(REALM_CORE_SUBMODULE_BUILD ON CACHE BOOL "Embedded Realm storage engine" FORCE)
set(REALM_BUILD_LIB_ONLY ON CACHE BOOL "No Realm tools" FORCE)
set(REALM_NO_TESTS ON CACHE BOOL "No upstream tests in release builds" FORCE)
set(REALM_ENABLE_ENCRYPTION OFF CACHE BOOL "osu local library is unencrypted" FORCE)
set(REALM_ENABLE_GEOSPATIAL OFF CACHE BOOL "No spatial queries in song enumeration" FORCE)
set(REALM_FORCE_OPENSSL OFF CACHE BOOL "No OpenSSL dependency" FORCE)
set(REALM_VERSION "20.1.2")
FetchContent_Declare(ttp_osu_realm
  URL https://github.com/realm/realm-core/archive/refs/tags/v20.1.2.tar.gz
  URL_HASH SHA256=c1c412319eeff021e98baf1a3857a1f8b330990b3052dcb8ca06ee7f3cc72896
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR ttplayer-populate-only)
# GitHub source archives do not contain git submodules. Match the exact SHA-2
# revision recorded by Realm's v20.1.2 gitlink, with its own archive digest.
FetchContent_Declare(ttp_osu_realm_sha2
  URL https://github.com/kalven/sha-2/archive/0e9aebf34101c6aa89355fd76ac9cd886735dee1.tar.gz
  URL_HASH SHA256=5d102d219a8054784d2b92d9816303e6f3dd6d5f102000b0df3bfe2583f298ca
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR ttplayer-populate-only)
FetchContent_MakeAvailable(ttp_osu_realm ttp_osu_realm_sha2)
foreach(_sha2_file sha224.cpp sha224.hpp sha256.cpp sha256.hpp)
  configure_file("${ttp_osu_realm_sha2_SOURCE_DIR}/${_sha2_file}"
    "${ttp_osu_realm_SOURCE_DIR}/src/external/sha-2/${_sha2_file}" COPYONLY)
endforeach()
add_subdirectory("${ttp_osu_realm_SOURCE_DIR}" "${ttp_osu_realm_BINARY_DIR}" EXCLUDE_FROM_ALL)

# Realm has legitimate typeid use. Keep it local to Realm and its adapter;
# the rest of the optional DLL retains the size-first /GR- build profile.
target_compile_options(Storage PRIVATE /GR)
target_compile_definitions(Storage PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0602 WINVER=0x0602)

# Realm's headers name FILE_ID_INFO and condition-variable APIs. Compile the
# adapter separately, with the same SDK declaration level as Storage, rather
# than inheriting the visual DLL's _WIN32_WINNT=0x0501. Runtime compatibility is
# provided by the existing XP CRT/thunk link profile and checked on the final
# DLL; its subsystem/OS version remain 5.1.
add_library(ttp_osu_realm_adapter OBJECT "${CMAKE_CURRENT_LIST_DIR}/../src/realm_library.cpp")
target_include_directories(ttp_osu_realm_adapter PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../src" "${CMAKE_CURRENT_LIST_DIR}/../include")
target_link_libraries(ttp_osu_realm_adapter PRIVATE Realm::Storage)
target_compile_definitions(ttp_osu_realm_adapter PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0602 WINVER=0x0602 _CRT_SECURE_NO_WARNINGS)
target_compile_options(ttp_osu_realm_adapter PRIVATE /GR /EHsc /utf-8 /W4 /permissive- /arch:SSE2)

function(ttp_osu_attach_realm target)
  target_sources(${target} PRIVATE $<TARGET_OBJECTS:ttp_osu_realm_adapter>)
  target_link_libraries(${target} PRIVATE Realm::Storage shell32)
endfunction()
