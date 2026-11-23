#include "androidoutput.h"

#include <QMetaObject>
#include <QString>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#include <QSettings>
#include <jni.h>
#endif

namespace hritapp {

AndroidOutput& AndroidOutput::instance() {
    static AndroidOutput output;
    return output;
}

AndroidOutput::AndroidOutput(QObject* parent) : QObject(parent) {
#ifdef Q_OS_ANDROID
    QSettings settings;
    treeUri_ = settings.value(QStringLiteral("androidOutputTreeUri")).toString();
    display_ = settings.value(QStringLiteral("androidOutputTreeName")).toString();
#endif
}

bool AndroidOutput::isAndroid() const {
#ifdef Q_OS_ANDROID
    return true;
#else
    return false;
#endif
}

bool AndroidOutput::pickDirectory(QWidget*) {
#ifdef Q_OS_ANDROID
    QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (!activity.isValid()) return false;
    activity.callMethod<void>("pickOutputTree", "()V");
    return true;
#else
    return false;
#endif
}

bool AndroidOutput::exportDirectory(const QString& localDir, QString& error) {
#ifdef Q_OS_ANDROID
    if (treeUri_.isEmpty()) {
        error = QStringLiteral("尚未选择输出目录。");
        return false;
    }
    QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (!activity.isValid()) {
        error = QStringLiteral("Android Activity 不可用。");
        return false;
    }
    const QJniObject tree = QJniObject::fromString(treeUri_);
    const QJniObject dir = QJniObject::fromString(localDir);
    const jboolean ok = activity.callMethod<jboolean>(
        "exportToTree", "(Ljava/lang/String;Ljava/lang/String;)Z", tree.object<jstring>(), dir.object<jstring>());
    if (!ok) {
        error = QStringLiteral("无法写入所选目录；请确认该目录仍有写入权限和足够空间。");
        return false;
    }
    return true;
#else
    Q_UNUSED(localDir);
    Q_UNUSED(error);
    return true;
#endif
}

void AndroidOutput::setTreeUri(const QString& uri) {
#ifdef Q_OS_ANDROID
    if (uri.isEmpty() || !uri.startsWith(QLatin1String("content://"))) {
        treeUri_.clear();
        display_.clear();
        QSettings settings;
        settings.remove(QStringLiteral("androidOutputTreeUri"));
        settings.remove(QStringLiteral("androidOutputTreeName"));
        emit treeUriChanged(display_);
        return;
    }
    treeUri_ = uri;
    QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (activity.isValid()) {
        const QJniObject arg = QJniObject::fromString(uri);
        const QJniObject name = activity.callObjectMethod(
            "treeDisplayName", "(Ljava/lang/String;)Ljava/lang/String;", arg.object<jstring>());
        display_ = name.isValid() ? name.toString() : uri;
    } else {
        display_ = uri;
    }
    if (display_.isEmpty()) display_ = uri;
    QSettings settings;
    settings.setValue(QStringLiteral("androidOutputTreeUri"), treeUri_);
    settings.setValue(QStringLiteral("androidOutputTreeName"), display_);
    emit treeUriChanged(display_);
#else
    Q_UNUSED(uri);
#endif
}

}  // namespace hritapp

#ifdef Q_OS_ANDROID
extern "C" JNIEXPORT void JNICALL
Java_com_nadirstation_gk2ahrit_MainActivity_nativeTreeSelected(JNIEnv* env, jclass, jstring uri, jint) {
    if (!uri) return;
    const char* chars = env->GetStringUTFChars(uri, nullptr);
    const QString value = QString::fromUtf8(chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(uri, chars);
    QMetaObject::invokeMethod(
        &hritapp::AndroidOutput::instance(),
        [value] { hritapp::AndroidOutput::instance().setTreeUri(value); }, Qt::QueuedConnection);
}
#endif
