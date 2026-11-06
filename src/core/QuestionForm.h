#pragma once
#include <QJsonArray>
#include <QJsonObject>
namespace ade {
// Value-only form state. Tokens and turn ownership are enforced by AgentEngine.
class QuestionForm {
public:
  static QJsonObject toolDefinition();
  static bool validate(const QJsonObject &definition, QString *error = nullptr);
  static QJsonObject defaults(const QJsonArray &fields,
                              const QJsonObject &saved = {});
  static bool validateAnswers(const QJsonArray &fields,
                              const QJsonObject &answers, bool requireComplete,
                              QString *error = nullptr);
  bool begin(const QJsonObject &definition, QString *error = nullptr);
  bool submit(int expectedStep, const QJsonObject &answers,
              QString *error = nullptr);
  bool updateDraft(int expectedStep, const QJsonObject &answers,
                   QString *error = nullptr);
  bool back(int expectedStep);
  bool completed() const {
    return m_step == m_definition["steps"].toArray().size();
  }
  int step() const { return m_step; }
  QJsonObject snapshot(const QString &token) const;
  QJsonObject result(const QString &reason = {}) const;

private:
  QJsonArray fields() const;
  QJsonObject m_definition, m_answers, m_draft;
  int m_step = 0;
};
} // namespace ade
