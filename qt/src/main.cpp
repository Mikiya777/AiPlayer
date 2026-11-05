#include "ui/MediaLibrary.h"
#include "ui/PlaybackController.h"
#include "video/VideoItem.h"
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QSGRendererInterface>
#include <QSurfaceFormat>
#include <QTimer>

int main(int argc, char** argv)
{
    // 所有启动入口共享同一组默认值；显式环境变量仍可用于性能对照。
    const auto setDefault = [](const char* name, const char* value) {
        if (!qEnvironmentVariableIsSet(name)) qputenv(name, QByteArray(value));
    };
#ifdef Q_OS_MACOS
    setDefault("AIPLAYER_VIDEO_DECODER", "auto");
    setDefault("AIPLAYER_RECORD_ENCODER", "h264_videotoolbox");
#endif
#ifdef AIPLAYER_HAVE_LIBYUV
    setDefault("AIPLAYER_RECORD_CONVERTER", "libyuv");
#endif
    setDefault("AIPLAYER_RECORD_READBACK", "direct");

    // QQuickFramebufferObject 依赖 OpenGL；必须在创建应用/窗口前确定后端。
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapInterval(1); // VSync 仅协调屏幕刷新，不能代替音画同步。
    QSurfaceFormat::setDefaultFormat(format);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("AiPlayer"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("media"), QStringLiteral("启动后播放的本地媒体文件"), QStringLiteral("[media]"));
    parser.addOption({QStringLiteral("quit-at-end"), QStringLiteral("播放结束自动退出")});
    parser.addOption({QStringLiteral("seek-ms"), QStringLiteral("测试：启动后跳转到指定毫秒"), QStringLiteral("ms")});
    parser.addOption({QStringLiteral("pause-demo"), QStringLiteral("测试：自动暂停并恢复")});
    parser.addOption({QStringLiteral("capture"), QStringLiteral("测试：保存实际 QML 窗口截图"), QStringLiteral("png")});
    parser.process(app);

    qmlRegisterType<PLAYER::VideoItem>("AiPlayer", 1, 0, "VideoItem");
    qmlRegisterUncreatableType<PLAYER::PlaybackController>(
        "AiPlayer", 1, 0, "PlaybackController", QStringLiteral("由 C++ 创建"));

    PLAYER::PlaybackController player;
    PLAYER::MediaLibrary mediaLibrary;
    if (parser.isSet(QStringLiteral("quit-at-end"))) {
        QObject::connect(&player, &PLAYER::PlaybackController::PlaybackEnded,
                         &app, &QCoreApplication::quit);
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("playbackController"), &player);
    engine.rootContext()->setContextProperty(QStringLiteral("mediaLibrary"), &mediaLibrary);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl(QStringLiteral("qrc:/ui/Main.qml")));
    if (engine.rootObjects().isEmpty()) return 1;

    if (!parser.positionalArguments().isEmpty()) {
        const QUrl url = QUrl::fromLocalFile(parser.positionalArguments().first());
        mediaLibrary.AddFiles({url});
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openMedia",
                                  Q_ARG(QVariant, QVariant::fromValue(url)));
    }
    if (parser.isSet(QStringLiteral("seek-ms"))) {
        QTimer::singleShot(450, &player, [&player, &parser] {
            player.SetPosition(parser.value(QStringLiteral("seek-ms")).toLongLong());
        });
    }
    if (parser.isSet(QStringLiteral("pause-demo"))) {
        QTimer::singleShot(300, &player, &PLAYER::PlaybackController::Pause);
        QTimer::singleShot(800, &player, &PLAYER::PlaybackController::Play);
    }
    if (parser.isSet(QStringLiteral("capture"))) {
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        const QString path = parser.value(QStringLiteral("capture"));
        QTimer::singleShot(1100, window, [window, path] { window->grabWindow().save(path); });
    }
    return app.exec();
}
