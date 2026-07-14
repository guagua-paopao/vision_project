#include "main_window.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VisionProjectDemo"));
    QCoreApplication::setApplicationName(QStringLiteral("PeopleFlowQtClient"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("People Flow V1 演示客户端"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption baseUrlOption({QStringLiteral("u"), QStringLiteral("base-url")},
                                     QStringLiteral("People Flow HTTP 服务地址"),
                                     QStringLiteral("url"));
    parser.addOption(baseUrlOption);
    parser.process(application);

    MainWindow window(parser.value(baseUrlOption));
    window.show();
    return application.exec();
}
