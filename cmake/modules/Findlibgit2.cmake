# Findlibgit2 — locates the static libgit2 built by deps/+libgit2 (libgit2
# 1.9 installs only a pkg-config file, which is unusable on MSVC).
# Provides imported target libgit2::libgit2.
find_path(LIBGIT2_INCLUDE_DIR NAMES git2.h)
find_library(LIBGIT2_LIBRARY NAMES git2 libgit2)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(libgit2 REQUIRED_VARS LIBGIT2_LIBRARY LIBGIT2_INCLUDE_DIR)

if (libgit2_FOUND AND NOT TARGET libgit2::libgit2)
    add_library(libgit2::libgit2 UNKNOWN IMPORTED)
    set_target_properties(libgit2::libgit2 PROPERTIES
        IMPORTED_LOCATION "${LIBGIT2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBGIT2_INCLUDE_DIR}")
endif ()
mark_as_advanced(LIBGIT2_INCLUDE_DIR LIBGIT2_LIBRARY)
