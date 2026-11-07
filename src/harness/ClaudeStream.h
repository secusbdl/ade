#pragma once
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QStringList>

namespace ade {
// Value-only presentation projection of the official Claude stream-json frames.
// Signatures and provider replay fields never enter this display state.
class ClaudeStream {
public:
  void reset(const QString &turnId);
  bool consume(const QJsonObject &frame, QJsonArray *updates, QString *error);
  QJsonArray finish(const QString &status);
  QJsonArray snapshot() const;

private:
  struct Block {
    QString type, id, name, text, argumentText, state, parent, message;
    QJsonObject arguments;
    bool hidden = false, closed = false, terminal = false, inputStarted = false;
    bool streamed = false, finalized = false;
    QByteArray finalDigest;
  };
  bool applyBlock(const QString &parent, const QString &message, int index,
                  const QJsonObject &value, bool final, QJsonArray *updates,
                  QString *error);
  bool applyFinal(const QJsonObject &frame, const QString &parent,
                  QJsonArray *updates, QString *error);
  bool updateText(Block &block, const QString &text, bool delta,
                  QJsonArray *updates, QString *error);
  void tool(Block &block, QJsonArray *updates);
  void close(Block &block, const QString &state, QJsonArray *updates);
  void append(QJsonArray *updates, QJsonObject value) const;
  QHash<QString, Block> m_blocks;
  QHash<QString, QString> m_current, m_tools;
  QHash<QString, QMap<int, QString>> m_messageBlocks;
  QHash<QString, QByteArray> m_finalFrames;
  QStringList m_order;
  QString m_turnId;
  qsizetype m_chars = 0;
};
} // namespace ade
