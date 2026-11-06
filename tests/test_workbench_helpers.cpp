#include "ui/SidebarSplitter.h"
#include "ui/WorkbenchSplitter.h"
#include <QPointer>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class WorkbenchHelpersTest final : public QObject {
    Q_OBJECT
  private slots:
    void hiddenResizeCollapseCallbackMayDeleteOwner() {
        QTemporaryDir directory;
        const auto ini = directory.filePath("window.ini");
        {
            QSettings settings(ini, QSettings::IniFormat);
            settings.setValue("sidebarCollapsed", true);
        }
        QPointer<SidebarSplitter> splitter = new SidebarSplitter(ini);
        splitter->addWidget(new QWidget);
        splitter->addWidget(new QWidget);
        splitter->setAttribute(Qt::WA_DontShowOnScreen);
        splitter->resize(1000, 600);
        splitter->show();
        QCoreApplication::processEvents();
        splitter->hide();
        splitter->resize(350, 500);
        connect(splitter, &SidebarSplitter::collapsedChanged, this, [&] { delete splitter.data(); });
        splitter->setCollapsed(false);
        QVERIFY(splitter.isNull());
    }
    void invalidPercentagesStayFinite() {
        QTemporaryDir directory;
        const auto ini = directory.filePath("window.ini");
        {
            QSettings settings(ini, QSettings::IniFormat);
            settings.setValue("contentPanelPercent", "nan");
            settings.setValue("fileTreePanelPercent", 1e300);
        }
        WorkbenchSplitter splitter(ini);
        splitter.initialize(new QWidget, new QWidget, new QWidget);
        splitter.resize(900, 500);
        splitter.setHasContent(true);
        splitter.setAttribute(Qt::WA_DontShowOnScreen);
        splitter.show();
        QCoreApplication::processEvents();
        QVERIFY(qIsFinite(splitter.contentPercent()));
        QCOMPARE(splitter.contentPercent(), 70.0);
        const auto sizes = splitter.sizes();
        QVERIFY(sizes[0] > 0 && sizes[1] > 0);
    }
};
QTEST_MAIN(WorkbenchHelpersTest)
#include "test_workbench_helpers.moc"
