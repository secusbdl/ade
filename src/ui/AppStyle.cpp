#include "AppStyle.h"
#include "AppFonts.h"
#include <QApplication>
#include <QFont>
namespace ade {
void initializeAppStyle(QApplication& app){
    const auto fonts=AppFonts::load();QFont body(fonts.body);body.setPixelSize(13);app.setFont(body);app.setStyle("Fusion");
    app.setStyleSheet(R"(
        QWidget { color:#292a2d; }
        QWidget#appSurface,QWidget#sidebar { background:#eff4fa; }
        QWidget#chatSurface { background:#fdfdfd; border-top-left-radius:14px; }
        QWidget#sideChatPane { background:#fdfdfd; }
        QTextBrowser,QPlainTextEdit { background:transparent; border:0; selection-background-color:#dbe6ff; }
        QListWidget#agentList { background:transparent; border:0; outline:0; }
        QListWidget#agentList::item { padding:2px 6px; border-radius:6px; }
        QListWidget#agentList::item:selected { background:#e2e8ee; color:#22262b; }
        QListWidget#agentList::item:hover { background:#e8edf2; }
        QLineEdit { border:1px solid #e4e6e8; background:#fafafa; border-radius:5px; padding:7px; }
        QLineEdit#agentSearch { background:#e5ebf1; border:0; }
        QPushButton { background:#f2f2f2; border:1px solid #e6e6e6; border-radius:6px; padding:7px 12px; }
        QPushButton:hover { background:#e8ebef; }
        QPushButton#newAgentButton { text-align:left; background:transparent; border:0; padding:7px 5px; font-size:14px; }
        QPushButton#newAgentButton:hover { background:#e2e8ee; }
        QToolButton { border:0; border-radius:5px; padding:3px; color:#6a6d72; }
        QToolButton:hover { background:#e9edf2; }
        QToolButton#sendButton { background:#7193ff; border-radius:16px; }
        QToolButton#sendButton:hover { background:#4b75f8; }
        QToolButton#sendButton:disabled { background:#c2cdf2; }
        QToolButton#stopButton:enabled { background:#ededed; border-radius:16px; }
        QComboBox { border:0; background:transparent; padding:4px; color:#64676c; }
        QComboBox::drop-down { border:0; width:16px; }
        QComboBox QAbstractItemView { background:#fff; border:1px solid #ddd; padding:4px; }
        QWidget#composerBox { background:#fdfdfd; border:1px solid #e4e5e7; border-radius:6px; }
        QWidget#composerBox QPlainTextEdit,QWidget#composerBox QToolButton,QWidget#composerBox QComboBox { border:0; }
        QSplitter::handle { background:#e1e1e1; }
        QTabWidget::pane { border:0; background:#fff; }
        QTabBar::tab { background:#fafafa; border:0; padding:8px 12px; margin:2px; border-radius:6px; min-width:75px; }
        QTabBar::tab:selected { background:#eeeff1; }
        QScrollBar:vertical { background:transparent; width:9px; margin:0; }
        QScrollBar::handle:vertical { background:#d1d3d6; border-radius:4px; min-height:35px; }
        QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }
        QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical { background:transparent; }
        QLabel[muted="true"] { color:#7b7f85; font-size:12px; }
        QDialog { background:#fcfcfc; }
    )");
}
}
