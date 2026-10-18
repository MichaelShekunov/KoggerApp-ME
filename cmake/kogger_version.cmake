# Writes generated/kogger_revision.h: the build's version as people see it, "ME <resources/version.txt>
# <the build's local date and time, dd.MM.yyyy HH:mm>" -- "ME 1.0.3 18.10.2026 10:23" (the owner,
# 2026-10-18: a date and time, not a commit hash) -- and the commit's 10-character revision the About
# page shows. Run on every build (cmake -P, the kogger_version target in CMakeLists.txt), so the time
# is this build's; configure_file rewrites the header only when it changes, and only
# src/build_version.cpp includes it. scripts/win_package_release.ps1 reads the same line back for the
# installer, so the setup and the title agree.
#
# In: SRC_DIR (the source tree), OUT (the header), GIT (git's path; empty: no revision).

set(_ver "0.0.0")
if(EXISTS "${SRC_DIR}/resources/version.txt")
    file(STRINGS "${SRC_DIR}/resources/version.txt" _ver LIMIT_COUNT 1)
    string(STRIP "${_ver}" _ver)
endif()

set(KOGGER_GIT_REVISION "")
if(GIT AND EXISTS "${SRC_DIR}/.git")
    execute_process(COMMAND "${GIT}" -C "${SRC_DIR}" rev-parse --short=10 HEAD
        OUTPUT_VARIABLE KOGGER_GIT_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()

string(TIMESTAMP _when "%d.%m.%Y %H:%M")
set(KOGGER_VERSION_FULL "ME ${_ver} ${_when}")

configure_file("${SRC_DIR}/src/kogger_revision.h.in" "${OUT}" @ONLY)
