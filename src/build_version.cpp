#include "build_version.h"

#include "kogger_revision.h"

QString koggerVersionFull()
{
    return QStringLiteral(KOGGER_VERSION_FULL);
}

QString koggerGitRevision()
{
    return QStringLiteral(KOGGER_GIT_REVISION);
}
