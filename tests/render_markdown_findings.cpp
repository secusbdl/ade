#include "ui/AppFonts.h"
#include "ui/MarkdownParts.h"
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
using namespace ade;
int main(int argc,char **argv){
    QApplication app(argc,argv);app.setStyle("Fusion");
    const auto fonts=AppFonts::load();QFont body(fonts.body);body.setPixelSize(14);app.setFont(body);
    const QString directory=app.arguments().value(1);if(directory.isEmpty()||!QDir().mkpath(directory))return 2;
    QList<QWidget*> views;
    const bool dark=app.arguments().value(2)=="dark";
    const int width=app.arguments().value(3,"768").toInt();
    {
        auto *surface=new QWidget;surface->setObjectName("conversationView");surface->resize(width,620);
        auto palette=surface->palette();palette.setColor(QPalette::Window,dark?QColor("#1c2028"):QColor("#fdfdfd"));palette.setColor(QPalette::Base,palette.color(QPalette::Window));
        palette.setColor(QPalette::Text,dark?QColor("#e1e5ec"):QColor("#292a2d"));palette.setColor(QPalette::WindowText,palette.color(QPalette::Text));palette.setColor(QPalette::ButtonText,palette.color(QPalette::Text));
        app.setPalette(palette);app.setStyleSheet("QWidget{color:"+palette.color(QPalette::Text).name()+";}");
        surface->setPalette(palette);surface->setAutoFillBackground(true);
        auto *layout=new QVBoxLayout(surface);layout->setContentsMargins(20,18,20,18);layout->setSpacing(16);
        auto *title=new QLabel("Native Markdown · completed response",surface);title->setWordWrap(true);layout->addWidget(title);
        auto *complete=new MarkdownParts(surface);complete->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);layout->addWidget(complete);
        complete->updateText("Updated [](path:ws1/src/App.tsx) using [](color:rgba(32, 120, 220, 0.5)).\n\nOpen [](tab:tab_1) or [](shell:session_1). See [the guide][docs].\n\n   ```ts\n   const message = 'Ready';\n   console.log(message);\n   ```\n\n[docs]: https://example.invalid/guide\n",false);
        auto *streamTitle=new QLabel("Streaming path",surface);layout->addWidget(streamTitle);
        auto *streaming=new MarkdownParts(surface);streaming->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);layout->addWidget(streaming);streaming->updateText("Reading [](path:ws1/src/Compo",true);
        auto *stoppedTitle=new QLabel("Stopped code · Copy available",surface);layout->addWidget(stoppedTitle);
        auto *stopped=new MarkdownParts(surface);stopped->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);layout->addWidget(stopped);stopped->updateText("```cpp\nconst int answer = 42;",false);
        layout->addStretch();surface->setProperty("capture",QString("%1-%2.png").arg(dark?"dark":"light").arg(width));surface->show();views.append(surface);
    }
    QTimer::singleShot(250,&app,[&]{int result=0;for(auto *view:views)if(!view->grab().save(directory+'/'+view->property("capture").toString()))result=3;for(auto *view:views)delete view;app.exit(result);});
    return app.exec();
}
