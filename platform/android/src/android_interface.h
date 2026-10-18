/****************************************************************************
 *
 * Copyright (C) 2018 Pinecone Inc. All rights reserved.
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QString>
#include <QtCore/QLoggingCategory>

#include <functional>
#include <jni.h>

class QObject;

Q_DECLARE_LOGGING_CATEGORY(AndroidInterfaceLog)

namespace AndroidInterface
{
    bool cleanJavaException();
    jclass getActivityClass();
    void setNativeMethods();
    void jniLogDebug(JNIEnv *envA, jobject thizA, jstring messageA);
    void jniLogWarning(JNIEnv *envA, jobject thizA, jstring messageA);
    void jniStoragePermissionResult(JNIEnv *envA, jobject thizA, jboolean grantedA);
    void jniOpenFile(JNIEnv *envA, jobject thizA, jstring uriA);
    bool checkStoragePermissions();
    void setStoragePermissionHandler(QObject *context, std::function<void(bool)> handler);
    /// Files other apps open with KoggerApp or share to it (a content:// URL, docs/map-export.md),
    /// given to `handler` on `context`'s thread. Those that came before it was set (the app was
    /// started by one) are given to it when it is set.
    void setOpenFileHandler(QObject *context, std::function<void(const QString&)> handler);
    /// Opens the system share sheet for a file. False when it cannot be offered.
    bool shareFile(const QString &path, const QString &mimeType, const QString &chooserTitle);
    QString getSDCardPath();
    void setKeepScreenOn(bool on);
    void moveTaskToBack();
    /// Starts (true) or stops (false) KoggerLinkService, the foreground service that
    /// keeps a device link alive while the app is not on screen.
    void setLinkServiceActive(bool active);

    constexpr const char *kJniKoggerActivityClassName = "org/kogger/koggerapp/KoggerActivity";
};
