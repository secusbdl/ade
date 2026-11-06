#pragma once
#include "attachments/FileTransformer.h"
#include <functional>
#include <QCryptographicHash>
#include <QJsonDocument>

namespace ade {
// Host-owned immutable observation. Never serialize bytes/entries into chat history.
struct FileReadCapture {
    QString tool;
    QString inputHash;
    FileDescriptor descriptor;
    FileReadParams params;
    bool directory = false;
    double capturedDepth = 0;
    QByteArray bytes;
    QList<FileTreeEntry> entries;
};
using FileReadCommitter = std::function<QJsonObject(const FileReadCapture &, const std::atomic_bool *, QString *)>;
// A host-scoped reader copied into a worker. Must capture values, never a QObject.
using FileReadSource = std::function<bool(const QString &, const QString &, const FileReadParams &,
    FileReadCapture *, const std::atomic_bool *, QString *)>;
inline QString fileReadInputHash(const QString& tool,const QJsonObject& arguments){
    return QString::fromLatin1(QCryptographicHash::hash(tool.toUtf8()+'\0'+QJsonDocument(arguments).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
}
