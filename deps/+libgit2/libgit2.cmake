# libgit2 — used by the profile-sync feature (src/slic3r/Utils/ProfileSync*).
#
# SSH transport is "exec": libgit2 shells out to the system `ssh` binary, so the
# user's existing ~/.ssh config, keys and agent are used as-is on every platform
# (Windows 10+ ships OpenSSH). This avoids a libssh2 + OpenSSL dependency chain.
# HTTPS uses the native TLS stack on each platform, mirroring what CURL does here.
set(_libgit2_platform_flags "")
if (WIN32)
  list(APPEND _libgit2_platform_flags -DUSE_HTTPS:STRING=WinHTTP)
elseif (APPLE)
  list(APPEND _libgit2_platform_flags -DUSE_HTTPS:STRING=SecureTransport)
else ()
  list(APPEND _libgit2_platform_flags -DUSE_HTTPS:STRING=OpenSSL)
endif ()

add_cmake_project(libgit2
  URL       https://github.com/libgit2/libgit2/archive/refs/tags/v1.9.1.tar.gz
  URL_HASH  SHA256=14cab3014b2b7ad75970ff4548e83615f74d719afe00aa479b4a889c1e13fc00
  # Don't flash a console window for the spawned ssh.exe on Windows.
  PATCH_COMMAND ${PATCH_CMD} ${CMAKE_CURRENT_LIST_DIR}/0001-no-console-window.patch
  CMAKE_ARGS
    -DBUILD_SHARED_LIBS:BOOL=OFF
    -DBUILD_TESTS:BOOL=OFF
    -DBUILD_CLI:BOOL=OFF
    -DBUILD_EXAMPLES:BOOL=OFF
    -DBUILD_FUZZERS:BOOL=OFF
    -DUSE_SSH:STRING=exec
    -DUSE_NTLMCLIENT:BOOL=OFF
    -DUSE_GSSAPI:BOOL=OFF
    -DUSE_HTTP_PARSER:STRING=builtin
    -DREGEX_BACKEND:STRING=builtin
    -DUSE_BUNDLED_ZLIB:BOOL=OFF
    -DLINK_WITH_STATIC_LIBRARIES:BOOL=ON
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    ${_libgit2_platform_flags}
)

set(DEP_libgit2_DEPENDS ZLIB)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND DEP_libgit2_DEPENDS OpenSSL)
endif ()
