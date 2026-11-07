#include "ui/FileDiffView.h"
#include <QtTest>
using namespace ade;
class DiffPreviewEdge : public QObject {
    Q_OBJECT
private slots:
    void truncationOnlyAfterDroppedRows_data() {
        QTest::addColumn<int>("lines"); QTest::addColumn<bool>("truncated");
        QTest::newRow("below") << 1198 << false;
        QTest::newRow("exact") << 1199 << false;
        QTest::newRow("above") << 1200 << true;
    }
    void truncationOnlyAfterDroppedRows() {
        QFETCH(int, lines); QFETCH(bool, truncated);
        QStringList content; for (int i=0;i<lines;++i)content.append(QString("line %1").arg(i));
        const auto result = FileDiffView::unifiedPreview(QString(""),content.join('\n'));
        QCOMPARE(result.contains("Preview truncated"),truncated);
        QCOMPARE(result.split('\n').size(),qMin(lines+1,1200)+(truncated?1:0));
    }
};
QTEST_MAIN(DiffPreviewEdge)
#include "test_diff_preview_edge.moc"
