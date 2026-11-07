#include "ShellProtocol.h"
#include <QSet>
#include <QUrl>
#include <cmath>
namespace ade {
bool ShellWaitPolicy::parse(const QJsonObject &args, ShellWaitPolicy *policy,
                            QString *error) {
  if (error)
    error->clear();
  auto reject = [&](const QString &text) {
    if (error)
      *error = text;
    return false;
  };
  const auto value = args["wait_until"];
  ShellWaitPolicy out;
  if (!value.isUndefined() && !value.isObject())
    return reject("wait_until must be an object.");
  if (value.isUndefined()) {
    out.timeoutMs = args.contains("stdin") ? 5000 : 10000;
    *policy = out;
    return true;
  }
  const auto wait = value.toObject();
  if (wait.contains("exited") && !wait["exited"].isBool())
    return reject("wait_until.exited must be boolean.");
  const bool exited = wait["exited"].toBool();
  out.timeoutMs = exited ? 300000 : 15000;
  out.idleMs = exited ? 15000 : 5000;
  for (const auto *key : {"timeout_ms", "idle_ms"})
    if (wait.contains(key)) {
      const auto v = wait[key];
      if (!v.isDouble() || !std::isfinite(v.toDouble()) ||
          v.toDouble() != std::floor(v.toDouble()))
        return reject("Shell wait limits must be integers.");
      const int lower = QString(key) == "timeout_ms" ? 1 : 0,
                upper = QString(key) == "timeout_ms" ? (exited ? 300000 : 60000)
                                                     : 60000;
      if (v.toDouble() < lower || v.toDouble() > upper)
        return reject("Shell wait limits exceed the supported range.");
      if (QString(key) == "timeout_ms")
        out.timeoutMs = v.toInt();
      else
        out.idleMs = v.toInt();
    }
  if (wait.contains("output_pattern")) {
    if (!wait["output_pattern"].isString() ||
        wait["output_pattern"].toString().size() > 1024)
      return reject(
          "Shell output pattern must contain at most 1024 characters.");
    out.pattern = QRegularExpression("(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=256)" +
                                     wait["output_pattern"].toString());
    out.hasPattern = true;
    if (!out.pattern.isValid())
      return reject("Shell output pattern is not a valid regular expression.");
  }
  *policy = out;
  return true;
}
ShellProtocol::ShellProtocol(QString token) : m_token(std::move(token)) {}
QList<ShellProtocolEvent> ShellProtocol::feed(const QByteArray &bytes) {
  QList<ShellProtocolEvent> events;
  m_pending += bytes;
  auto output = [&](const QByteArray &text) {
    if (!text.isEmpty())
      events.append({ShellProtocolEvent::Output, text, {}, 0});
  };
  for (;;) {
    const auto start = m_pending.indexOf("\x1b]");
    if (start < 0) {
      int keep = m_pending.endsWith('\x1b') ? 1 : 0;
      output(m_pending.left(m_pending.size() - keep));
      m_pending = m_pending.right(keep);
      break;
    }
    if (start > 0) {
      output(m_pending.left(start));
      m_pending.remove(0, start);
    }
    const auto bel = m_pending.indexOf('\a', 2),
               st = m_pending.indexOf("\x1b\\", 2);
    const auto end = bel < 0 ? st : st < 0 ? bel : qMin(bel, st);
    if (end < 0) {
      if (m_pending.size() > 8192) {
        output(m_pending.left(2));
        m_pending.remove(0, 2);
        continue;
      }
      break;
    }
    const int terminator = end == bel ? 1 : 2;
    const auto osc = m_pending.mid(2, end - 2);
    m_pending.remove(0, end + terminator);
    if (osc.startsWith("133;D;")) {
      const auto pieces = osc.split(';');
      bool ok = false;
      const int code = pieces.size() == 4 ? pieces[2].toInt(&ok) : 0;
      if (ok && !m_token.isEmpty() && pieces[3] == m_token.toUtf8()) {
        const bool wasCommand = m_inCommand;
        m_inCommand = false;
        m_authenticatedPrompt = true;
        if (wasCommand && m_ready)
          events.append({ShellProtocolEvent::CommandDone, {}, {}, code});
      }
    } else if (osc == "133;C") {
      if (m_ready) {
        m_inCommand = true;
        m_authenticatedPrompt = false;
        events.append({ShellProtocolEvent::CommandStart, {}, {}, 0});
      }
    } else if (osc.startsWith("7;")) {
      if (m_authenticatedPrompt && !m_inCommand) {
        const QUrl url(QString::fromUtf8(osc.mid(2)));
        if (url.scheme() == "file" &&
            (url.host().isEmpty() || url.host() == "localhost")) {
          auto path = url.toLocalFile();
          if (path.startsWith("//localhost/", Qt::CaseInsensitive))
            path = path.mid(12);
          if (!path.isEmpty())
            events.append({ShellProtocolEvent::WorkingDirectory, {}, path, 0});
        }
      }
    } else if (osc == "133;A") {
      if (m_authenticatedPrompt && !m_ready) {
        m_ready = true;
        events.append({ShellProtocolEvent::Ready, {}, {}, 0});
      }
      m_authenticatedPrompt = false;
    } else if (!osc.startsWith("133;"))
      output("\x1b]" + osc +
             (terminator == 1 ? QByteArray("\a") : QByteArray("\x1b\\")));
  }
  return events;
}
QString ShellProtocol::expandStdinEscapes(const QString &text) {
  QString result;
  result.reserve(text.size());
  for (qsizetype i = 0; i < text.size(); ++i) {
    if (text[i] != QLatin1Char('\\') || i + 1 == text.size()) {
      result += text[i];
      continue;
    }
    const auto next = text[i + 1];
    if (next == QLatin1Char('x') && i + 3 < text.size()) {
      const auto hex = text.mid(i + 2, 2);
      if (QRegularExpression(QStringLiteral("^[0-9a-fA-F]{2}$"))
              .match(hex)
              .hasMatch()) {
        result += QChar(hex.toUShort(nullptr, 16));
        i += 3;
        continue;
      }
    }
    const QString codes = QStringLiteral("rntab\\");
    const QString values = QStringLiteral("\r\n\t\a\b\\");
    const auto index = codes.indexOf(next);
    if (index >= 0) {
      result += values[index];
      ++i;
    } else
      result += text[i];
  }
  return result;
}
QString ShellProtocol::powerShellIntegration(const QString &token) {
  // Adapted from pinned agent-shell PowerShell integration. The token is
  // generated by the host and contains only UUID hex; it is never exported.
  if (!QRegularExpression("^[a-zA-Z0-9_-]{16,128}$").match(token).hasMatch())
    return {};
  return QString::fromUtf8(R"PS(
$global:__ADEAgentShell = @{ BoundaryToken = '%1'; OriginalPrompt = $function:prompt; HasReadLine = $false; IsInExecution = $false; Initialized = $false }
Import-Module PSReadLine -ErrorAction SilentlyContinue
if (Get-Module -Name PSReadLine) {
  Set-PSReadLineOption -HistorySaveStyle SaveNothing
  $global:__ADEAgentShell.HasReadLine = $true
  $global:__ADEAgentShell.OriginalReadLine = $function:PSConsoleHostReadLine
} else {
  # ConsoleHost normally falls back internally when PSReadLine is unavailable.
  # Wrap that basic read explicitly so output starts after echo, for both
  # managed commands and raw stdin submitted at the prompt.
  $global:__ADEAgentShell.OriginalReadLine = { [Console]::ReadLine() }
}
function global:PSConsoleHostReadLine {
  $line = & $global:__ADEAgentShell.OriginalReadLine
  if ($null -ne $line) {
    $global:__ADEAgentShell.IsInExecution = $true
    [Console]::Write([string][char]27+']133;C'+[char]7)
  }
  $line
}
function global:prompt {
  $success = $global:?
  $code = 0
  if (!$success) { $code = 1 }
  if (!$global:__ADEAgentShell.Initialized -or !$global:__ADEAgentShell.HasReadLine -or $global:__ADEAgentShell.IsInExecution) {
    [Console]::Write([string][char]27+']133;D;'+$code+';'+$global:__ADEAgentShell.BoundaryToken+[char]7)
    $global:__ADEAgentShell.IsInExecution = $false
    $global:__ADEAgentShell.Initialized = $true
  }
  try { [Console]::Write([string][char]27+']7;'+([Uri]::new((Get-Location).ProviderPath)).AbsoluteUri+[char]7) } catch {}
  [Console]::Write([string][char]27+']133;A'+[char]7)
  $promptText = & $global:__ADEAgentShell.OriginalPrompt
  $promptText + [string][char]27+']133;B'+[char]7
}
)PS")
      .arg(token);
}
QProcessEnvironment
ShellProtocol::agentEnvironment(const QProcessEnvironment &source) {
  static const QSet<QString> blocked{"NODE_ENV",
                                     "BUILD_MODE",
                                     "POSTHOG_API_KEY",
                                     "POSTHOG_HOST",
                                     "STAGEWISE_CONSOLE_URL",
                                     "API_URL",
                                     "LLM_PROXY_URL",
                                     "UPDATE_SERVER_ORIGIN",
                                     "SUPABASE_URL",
                                     "SUPABASE_PUBLISHABLE_KEY",
                                     "__STAGEWISE_SHELL_INTEGRATION"};
  static const QSet<QString> exempt{"PATH",
                                    "HOME",
                                    "USER",
                                    "USERNAME",
                                    "LOGNAME",
                                    "SHELL",
                                    "TERM",
                                    "LANG",
                                    "LC_ALL",
                                    "LC_CTYPE",
                                    "TMPDIR",
                                    "TEMP",
                                    "TMP",
                                    "EDITOR",
                                    "VISUAL",
                                    "XDG_CONFIG_HOME",
                                    "XDG_DATA_HOME",
                                    "XDG_CACHE_HOME",
                                    "NPM_CONFIG_REGISTRY",
                                    "SYSTEMROOT",
                                    "WINDIR",
                                    "USERPROFILE",
                                    "HOMEDRIVE",
                                    "HOMEPATH",
                                    "LOCALAPPDATA",
                                    "APPDATA",
                                    "PROGRAMDATA",
                                    "PROGRAMFILES",
                                    "PROGRAMFILES(X86)",
                                    "COMMONPROGRAMFILES",
                                    "PATHEXT",
                                    "COMSPEC",
                                    "SYSTEMDRIVE",
                                    "PROCESSOR_ARCHITECTURE",
                                    "PROCESSOR_IDENTIFIER",
                                    "PROCESSOR_LEVEL",
                                    "PROCESSOR_REVISION",
                                    "NUMBER_OF_PROCESSORS",
                                    "OS",
                                    "COMPUTERNAME",
                                    "USERDOMAIN",
                                    "SESSIONNAME",
                                    "LOGONSERVER",
                                    "MSYSTEM",
                                    "CHERE_INVOKING",
                                    "PNPM_HOME",
                                    "NPM_CONFIG_PREFIX",
                                    "NPM_CONFIG_CACHE",
                                    "NODE_PATH",
                                    "NVM_HOME",
                                    "NVM_SYMLINK",
                                    "FNM_DIR",
                                    "FNM_MULTISHELL_PATH",
                                    "FNM_NODE_DIST_MIRROR",
                                    "FNM_VERSION_FILE_STRATEGY"};
  QProcessEnvironment result;
  for (const auto &key : source.keys()) {
    const auto upper = key.toUpper();
    if (blocked.contains(upper) || upper.startsWith("ELECTRON_") ||
        upper.startsWith("ELECTRON "))
      continue;
    bool sensitive = false;
    for (const auto *pattern : {"SECRET", "TOKEN", "KEY", "PASSWORD",
                                "CREDENTIAL", "AUTH", "PRIVATE"})
      sensitive |= upper.contains(pattern);
    if (sensitive && !exempt.contains(upper))
      continue;
    result.insert(upper == "PATH" ? QString("PATH") : key, source.value(key));
  }
  result.insert("HISTFILE", "NUL");
  result.insert("HISTSIZE", "0");
  result.insert("SAVEHIST", "0");
  result.insert("ADE_AGENT_SHELL", "1");
  return result;
}
} // namespace ade
