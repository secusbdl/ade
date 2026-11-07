#pragma once
#include <QHash>
#include <QJsonObject>
#include <QWidget>
#include <QUrl>
class QFrame;
class QLabel;
class QVBoxLayout;
namespace ade {
class TranscriptText;
struct QuestionAnswerDisplay {QString id,label,value;};
struct QuestionToolDisplay {QString summary;QList<QuestionAnswerDisplay> answers;bool truncated=false;};
// Read-only projection. Canonical answers and execution/approval authority are unchanged.
bool isNativeQuestionPart(const QJsonObject& part);
QuestionToolDisplay questionToolDisplay(const QJsonObject& part);
QString questionToolPlainText(const QJsonObject& part);
class QuestionToolPart final:public QWidget {
 Q_OBJECT
public:
 explicit QuestionToolPart(QWidget* parent=nullptr);
 void updatePart(const QJsonObject& part);
 void refreshAppearance();
signals:
 void linkActivated(QUrl url);
protected:
 void changeEvent(QEvent* event) override;
private:
 struct AnswerRow {QWidget* widget;TranscriptText* label;TranscriptText* value;};
 void colors();
 void fonts();
 QLabel* m_summary=nullptr;QFrame* m_answers=nullptr;QVBoxLayout* m_answerLayout=nullptr;QLabel* m_notice=nullptr;
 QHash<QString,AnswerRow> m_rows;QJsonObject m_part;quint64 m_epoch=0;bool m_initialized=false,m_coloring=false;
};
}
