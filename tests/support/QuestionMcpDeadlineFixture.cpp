#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <string>
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  std::string line;
  while (std::getline(std::cin, line)) {
    const auto message =
        QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
    if (message["method"] == "exit")
      return 0;
    if (message["method"] == "ping") {
      const QJsonObject reply{
          {"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", QJsonObject{}}};
      std::cout
          << QJsonDocument(reply).toJson(QJsonDocument::Compact).constData()
          << std::endl;
    }
  }
  return 0;
}
