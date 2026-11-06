#include "ui/AppFonts.h"
#include "ui/ChevronToolButton.h"
#include "ui/Icons.h"
#include <QDir>
#include <QHBoxLayout>
#include <QSignalSpy>
#include <QtTest>
using namespace ade;
namespace {
void theme(QWidget& widget, bool dark, int zoom) {
    auto font = qApp->font();
    font.setPixelSize(qRound(13 * zoom / 100.0));
    widget.setFont(font);
    auto palette = widget.palette();
    palette.setColor(QPalette::Window, dark ? QColor("#21242b") : QColor("#fdfdfd"));
    palette.setColor(QPalette::ButtonText, dark ? QColor("#bec3cb") : QColor("#6a6d72"));
    widget.setPalette(palette);
    widget.setAutoFillBackground(true);
    widget.setStyleSheet(
        QString(
            "QToolButton{border:0;border-radius:5px;padding:3px;color:%1;}QToolButton:hover{background:%2;}")
            .arg(dark ? "#bec3cb" : "#6a6d72", dark ? "#303846" : "#e9edf2"));
}
} // namespace
class ChevronTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        const auto families = AppFonts::load();
        QVERIFY(families.fullyLoaded());
        QFont font(families.body);
        font.setPixelSize(13);
        qApp->setFont(font);
        qApp->setStyle("Fusion");
    }
    void drawsWithoutGlyph_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("zoom");
        for (bool dark : {false, true})
            for (int zoom : {70, 100, 130})
                QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light").arg(zoom)))
                    << dark << zoom;
    }
    void drawsWithoutGlyph() {
        QFETCH(bool, dark);
        QFETCH(int, zoom);
        QWidget host;
        theme(host, dark, zoom);
        auto* layout = new QHBoxLayout(&host);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(20);
        auto* grouping = new ChevronToolButton;
        grouping->setText("Group by Age");
        grouping->setAutoRaise(true);
        layout->addWidget(grouping);
        auto* workspace = new ChevronToolButton;
        workspace->setText("layout-fixture");
        workspace->setIcon(icon("folder"));
        workspace->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        workspace->setAutoRaise(true);
        layout->addWidget(workspace);
        host.setAttribute(Qt::WA_DontShowOnScreen);
        host.adjustSize();
        host.show();
        QTest::qWait(10);
        for (auto* button : {grouping, workspace}) {
            QVERIFY(!button->text().contains(QChar(0x2304)));
            QVERIFY(!button->menu());
            const auto before = button->grab().toImage();
            const int reserve = qMax(10, qRound(button->fontMetrics().height() * .8));
            const int fullWidth = button->width();
            button->setChevronVisible(false);
            const auto without = button->grab().toImage();
            int changed = 0;
            for (int y = 0; y < before.height(); ++y)
                for (int x = qMax(0, fullWidth - reserve); x < fullWidth; ++x)
                    if (before.pixelColor(x, y) != without.pixelColor(x, y))
                        ++changed;
            QVERIFY2(changed >= 5, "Drawn suffix must produce pixels without any Unicode glyph.");
            button->setChevronVisible(true);
            QSignalSpy clicked(button, &QToolButton::clicked);
            QTest::mouseClick(button, Qt::LeftButton);
            QCOMPARE(clicked.size(), 1);
            QCOMPARE(button->popupMode(), QToolButton::DelayedPopup);
        }
        const auto output = qEnvironmentVariable("ADE_CHEVRON_RENDER_DIR");
        if (!output.isEmpty()) {
            QDir().mkpath(output);
            QVERIFY(host.grab().save(output +
                                     QString("/chevrons-%1-%2.png").arg(dark ? "dark" : "light").arg(zoom)));
        }
    }
    void disconnectedWorkspaceKeepsPlainAction() {
        ChevronToolButton button;
        button.setText("Connect workspace  +");
        button.setChevronVisible(false);
        QToolButton plain;
        plain.setText(button.text());
        QCOMPARE(button.sizeHint(), plain.sizeHint());
        QSignalSpy spy(&button, &QToolButton::clicked);
        button.click();
        QCOMPARE(spy.size(), 1);
        QVERIFY(!button.menu());
    }
};
QTEST_MAIN(ChevronTest)
#include "test_chevron.moc"
