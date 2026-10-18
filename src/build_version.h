#pragma once

#include <QString>

// The build's version as people see it: "ME 1.0.3 18.10.2026 10:23", the build's local date and time
// (cmake/kogger_version.cmake, on every build). The window's title ends with it.
QString koggerVersionFull();
// The commit's 10-character abbreviation; empty outside a git checkout.
QString koggerGitRevision();
