// Second-process client for test_instance_coordinator. Exit codes:
// 0 Forwarded, 10 Primary, 3 Refused, 4 Failed, 64 usage.
#include "ui/InstanceCoordinator.h"
#include <QCoreApplication>
#include <QDir>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    auto arguments = app.arguments().mid(1);
    const bool refuse = arguments.removeAll("--refuse") > 0;
    if (arguments.isEmpty()) return 64;
    const auto directory = arguments.takeFirst();
    ade::InstanceCoordinator coordinator(directory);
    QString error;
    const ade::InstanceRequest request{ade::InstanceCoordinator::filterUrls(arguments), QDir::currentPath()};
    switch (coordinator.acquire(request, 5000, &error, refuse)) {
    case ade::InstanceCoordinator::Role::Forwarded: return 0;
    case ade::InstanceCoordinator::Role::Primary: return 10;
    case ade::InstanceCoordinator::Role::Refused: return 3;
    case ade::InstanceCoordinator::Role::Failed: return 4;
    }
    return 4;
}
