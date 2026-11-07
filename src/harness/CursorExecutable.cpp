#include "CursorExecutable.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
namespace ade {
CursorExecutable CursorExecutable::resolve(const QString &requested,
                                           QString *error) {
  if (error)
    error->clear();
  auto reject = [&] {
    if (error)
      *error = "Select the installed Cursor Agent executable or its vendor "
               "agent launcher.";
    return CursorExecutable{};
  };
  QString path = requested;
  if (path.isEmpty()) {
    for (const auto &name :
         {QString("agent"), QString("agent.cmd"), QString("agent.ps1"),
          QString("cursor-agent.cmd")}) {
      path = QStandardPaths::findExecutable(name);
      if (!path.isEmpty())
        break;
    }
#ifdef Q_OS_WIN
    if (path.isEmpty())
      path = QDir(qEnvironmentVariable("LOCALAPPDATA"))
                 .filePath("cursor-agent/agent.ps1");
#endif
  } else if (!QFileInfo(path).isAbsolute())
    path = QStandardPaths::findExecutable(path);
  QFileInfo info(path);
  if (!info.exists())
    return reject();
  QString directory;
  if (info.isDir())
    directory = info.canonicalFilePath();
  else if (info.suffix().compare("exe", Qt::CaseInsensitive) == 0) {
    if (info.fileName().compare("node.exe", Qt::CaseInsensitive) == 0)
      return reject();
    CursorExecutable result;
    result.program = info.canonicalFilePath();
    result.arguments = {"acp"};
    result.identity = QString::fromLatin1(
        QCryptographicHash::hash(
            (result.program + "|" + QString::number(info.size()) + "|" +
             QString::number(info.lastModified().toMSecsSinceEpoch()))
                .toUtf8(),
            QCryptographicHash::Sha256)
            .toHex());
    return result;
  } else if (QStringList{"agent.ps1", "cursor-agent.ps1", "agent.cmd",
                         "cursor-agent.cmd"}
                 .contains(info.fileName().toLower()))
    directory = info.absolutePath();
  else
    return reject();
  if (!QFileInfo::exists(QDir(directory).filePath("node.exe"))) {
    const QDir versions(QDir(directory).filePath("versions"));
    static const QRegularExpression version(
        R"(^\d{4}\.\d{2}\.\d{2}(?:-\d{2}-\d{2}-\d{2})?-[a-f0-9]+$)");
    QStringList candidates;
    for (const auto &entry :
         versions.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
      if (version.match(entry.fileName()).hasMatch() &&
          QFileInfo::exists(
              QDir(entry.absoluteFilePath()).filePath("node.exe")) &&
          QFileInfo::exists(
              QDir(entry.absoluteFilePath()).filePath("index.js")))
        candidates.append(entry.fileName());
    candidates.sort();
    if (candidates.isEmpty())
      return reject();
    directory = versions.filePath(candidates.last());
  }
  QFileInfo node(QDir(directory).filePath("node.exe")),
      script(QDir(directory).filePath("index.js"));
  if (!node.isFile() || !script.isFile() || script.size() > 32 * 1024 * 1024)
    return reject();
  QFile file(script.absoluteFilePath());
  if (!file.open(QIODevice::ReadOnly))
    return reject();
  const auto bytes = file.read(32 * 1024 * 1024 + 1);
  if (bytes.size() > 32 * 1024 * 1024)
    return reject();
  CursorExecutable result;
  result.program = node.canonicalFilePath();
  result.entrypoint = script.canonicalFilePath();
  result.arguments = {result.entrypoint, "acp"};
  const auto fingerprint =
      result.program.toUtf8() + result.entrypoint.toUtf8() +
      QByteArray::number(node.size()) +
      QByteArray::number(node.lastModified().toMSecsSinceEpoch()) +
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
  result.identity = QString::fromLatin1(
      QCryptographicHash::hash(fingerprint, QCryptographicHash::Sha256)
          .toHex());
  return result;
}
} // namespace ade
