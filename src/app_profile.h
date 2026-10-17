#pragma once

#include <QString>
#include <QtGlobal>

/// The application's name: KoggerApp, or KoggerApp-<name> with KOGGER_PROFILE=<name>, a run whose
/// settings, logs and app data are its own (a test beside the user's open app). main() names the
/// application with it; code that runs before that (the global Core) or opens QSettings by name uses
/// it directly. A QSettings opened as ("KOGGER", "KoggerApp") wrote the user's settings from a profile
/// run: the 3D camera kept at exit was the last test's (2026-10-17).
inline QString koggerAppName()
{
    const QString profile = qEnvironmentVariable("KOGGER_PROFILE");
    return profile.isEmpty() ? QStringLiteral("KoggerApp") : QStringLiteral("KoggerApp-") + profile;
}
