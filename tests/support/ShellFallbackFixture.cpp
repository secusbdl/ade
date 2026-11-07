#include <QByteArray>
#include <QCoreApplication>
#include <QStringList>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace {
QString quote(const QString &value) {
  QString out = "\"";
  int slashes = 0;
  for (const auto c : value) {
    if (c == '\\') {
      ++slashes;
      continue;
    }
    if (c == '\"') {
      out += QString(slashes * 2 + 1, '\\');
      out += c;
    } else {
      out += QString(slashes, '\\');
      out += c;
    }
    slashes = 0;
  }
  out += QString(slashes * 2, '\\');
  return out + '\"';
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  auto args = app.arguments();
  args.removeFirst();
  const auto real = qEnvironmentVariable("ADE_TEST_REAL_POWERSHELL");
  if (real.isEmpty())
    return 121;
  const auto index = args.indexOf("-EncodedCommand");
  if (index < 0 || index + 1 >= args.size())
    return 122;
  const auto bytes = QByteArray::fromBase64(args[index + 1].toLatin1());
  if (bytes.size() % 2)
    return 123;
  auto source = QString::fromUtf16(
      reinterpret_cast<const char16_t *>(bytes.constData()), bytes.size() / 2);
  const QString line = "Import-Module PSReadLine -ErrorAction SilentlyContinue";
  if (!source.contains(line))
    return 124;
  source.replace(
      line,
      "Remove-Module PSReadLine -ErrorAction SilentlyContinue; Remove-Item "
      "Function:PSConsoleHostReadLine -ErrorAction SilentlyContinue");
  args[index + 1] = QString::fromLatin1(
      QByteArray(reinterpret_cast<const char *>(source.utf16()),
                 source.size() * 2)
          .toBase64());
  if (!args.contains("-NoProfile"))
    args.prepend("-NoProfile");
  QString command = quote(real);
  for (const auto &arg : args)
    command += ' ' + quote(arg);
  auto mutableCommand = command.toStdWString();
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(reinterpret_cast<LPCWSTR>(real.utf16()),
                      mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr,
                      nullptr, &startup, &process))
    return 125;
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 126;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  return int(code);
}
