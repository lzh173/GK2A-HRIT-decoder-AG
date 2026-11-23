// androidoutput.h - Android Storage Access Framework output directory.
//
// The decoder needs ordinary filesystem paths, while Android gives applications a tree URI. Decode
// into the app-private staging directory, then copy the finished tree into the directory the user
// picked. All SAF work lives in MainActivity.java; this class is the small Qt-side facade.
#pragma once

#include <QObject>
#include <QString>

class QWidget;

namespace hritapp {

class AndroidOutput : public QObject {
    Q_OBJECT
public:
    static AndroidOutput& instance();

    bool isAndroid() const;
    bool hasTreeUri() const { return !treeUri_.isEmpty(); }
    QString displayName() const { return display_; }

    // Opens ACTION_OPEN_DOCUMENT_TREE. The result arrives asynchronously through treeUriChanged().
    bool pickDirectory(QWidget* parent = nullptr);

    // Copies localDir (recursively) into the picked tree. Returns true on non-Android (nothing to do).
    bool exportDirectory(const QString& localDir, QString& error);

    // Called from the JNI callback in MainActivity.
    void setTreeUri(const QString& uri);

signals:
    void treeUriChanged(const QString& display);

private:
    explicit AndroidOutput(QObject* parent = nullptr);

    QString treeUri_;
    QString display_;
};

}  // namespace hritapp
