include(FetchContent)

# Shared archive cache
# Archives are downloaded once and reused across all build directories.
# Sources and build artifacts remain per-build (FETCHCONTENT_BASE_DIR default).
set(KL_DEPS_DOWNLOAD_CACHE "${CMAKE_SOURCE_DIR}/.deps"
        CACHE PATH "Shared cache for downloaded dependency archives")

# Offline / vendored archive directory
# Place archives here using their original URL filename (e.g. zlib-1.3.2.tar.gz)
# for fully offline builds.
set(KL_LOCAL_DEPS "" CACHE PATH
        "Directory with pre-downloaded archives for offline builds")

file(MAKE_DIRECTORY "${KL_DEPS_DOWNLOAD_CACHE}")

# Internal: extract full extension (.tar.gz, .tar.xz, .zip, …)
function(_kl_ext url out)
    if (url MATCHES "\\.(tar\\.[a-zA-Z0-9]+)$")
        set(${out} ".${CMAKE_MATCH_1}" PARENT_SCOPE)
    else ()
        cmake_path(GET url EXTENSION LAST_ONLY _e)
        set(${out} "${_e}" PARENT_SCOPE)
    endif ()
endfunction()

# kl_fetch(<name> <url> <sha256> [FetchContent_Declare options…])
#
# Resolves an archive and declares it as a local file:// URL so FetchContent
# never hits the network on repeat configures.
#
# Resolution order:
#   1. KL_LOCAL_DEPS/<url-basename>                       - vendor-supplied, used as-is
#   2. KL_DEPS_DOWNLOAD_CACHE/<name>-<12-char-hash><ext>  - shared download cache
function(kl_fetch name url sha256)
    if (NOT name OR NOT url OR NOT sha256)
        message(FATAL_ERROR "kl_fetch: name, url, and sha256 are all required")
    endif ()

    _kl_ext("${url}" _ext)
    cmake_path(GET url FILENAME _url_fname)

    # 1. Vendor directory
    if (KL_LOCAL_DEPS AND EXISTS "${KL_LOCAL_DEPS}/${_url_fname}")
        set(_archive "${KL_LOCAL_DEPS}/${_url_fname}")

        # 2. Shared download cache
    else ()
        string(SUBSTRING "${sha256}" 0 12 _hash_prefix)
        set(_archive "${KL_DEPS_DOWNLOAD_CACHE}/${name}-${_hash_prefix}${_ext}")

        file(LOCK "${_archive}.lock" TIMEOUT 300)

        if (EXISTS "${_archive}")
            file(SHA256 "${_archive}" _cached_hash)
            if (NOT _cached_hash STREQUAL "${sha256}")
                message(WARNING "kl_fetch: hash mismatch for cached ${name}, re-downloading")
                file(REMOVE "${_archive}")
            endif ()
        endif ()

        if (NOT EXISTS "${_archive}")
            message(STATUS "kl_fetch: downloading ${name}")
            message(STATUS "  url:  ${url}")
            message(STATUS "  dest: ${_archive}")
            file(DOWNLOAD "${url}" "${_archive}"
                    EXPECTED_HASH "SHA256=${sha256}"
                    SHOW_PROGRESS
                    STATUS _status
                    LOG _log
            )
            list(GET _status 0 _code)
            if (NOT _code EQUAL 0)
                file(REMOVE "${_archive}")
                file(LOCK "${_archive}.lock" RELEASE)
                message(FATAL_ERROR "kl_fetch: download failed for ${name}:\n${_log}")
            endif ()
        endif ()

        file(LOCK "${_archive}.lock" RELEASE)
    endif ()

    FetchContent_Declare(${name}
            URL "file://${_archive}"
            URL_HASH "SHA256=${sha256}"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            ${ARGN}
    )
endfunction()