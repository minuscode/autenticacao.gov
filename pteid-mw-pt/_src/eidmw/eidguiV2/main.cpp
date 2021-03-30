/*-****************************************************************************

 * Copyright (C) 2017-2021 Adriano Campos - <adrianoribeirocampos@gmail.com>
 * Copyright (C) 2017 André Guerreiro - <aguerreiro1985@gmail.com>
 * Copyright (C) 2019 João Pinheiro - <joao.pinheiro@caixamagica.pt>
 * Copyright (C) 2019 Miguel Figueira - <miguelblcfigueira@gmail.com>
 *
 * Licensed under the EUPL V.1.2

****************************************************************************-*/

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>
#include <QProcess>
#include "appcontroller.h"
#include "gapi.h"
#include "eidlib.h"
#include "singleapplication.h"
#include <QDesktopWidget> 
#include <QCommandLineParser> 
#include "pteidversions.h"

using namespace eIDMW;

const char *signSimpleDescription = "Open simple Signature submenu.";
const char *signAdvancedDescription = "Open advanced Signature submenu.";

int parseCommandlineAppArguments(QCommandLineParser *parser, GUISettings *settings) {

    QString modeDescription("Mode of the application. Possible values are:\n ");
    modeDescription.append("\"\" (empty): default mode\n");
    modeDescription.append("\"signSimple\": ").append(signSimpleDescription).append("\n");
    modeDescription.append("\"signAdvanced\": ").append(signAdvancedDescription);
    parser->addPositionalArgument("mode", modeDescription);

    const QCommandLineOption helpOption = parser->addHelpOption();
    const QCommandLineOption versionOption = parser->addVersionOption();

    // Graphics rendering options
    const QCommandLineOption softwareModeOption(QStringList() 
        << "s" << "software", "Graphics rendering with Software (OpenGL)");
    parser->addOption(softwareModeOption);
    const QCommandLineOption hardwareModeOption(QStringList() 
        << "c" << "card", "Graphics rendering with Hardware (Video card)");
    parser->addOption(hardwareModeOption);
#ifdef WIN32
    const QCommandLineOption direct3dModeOption(QStringList() 
        << "w" << "direct3d", "Graphics rendering with Software (Direct3D)");
    parser->addOption(direct3dModeOption);
#endif
    const QCommandLineOption testModeOption(QStringList() 
        << "t" << "test", "Enable test mode");
    parser->addOption(testModeOption);

    parser->parse(QCoreApplication::arguments());

    PTEID_Config sam_server(PTEID_PARAM_GENERAL_SAM_SERVER);
    // Default is production mode
    PTEID_Config::SetTestMode(false);
    sam_server.setString("pki.cartaodecidadao.pt:443");

    if (parser->isSet(testModeOption))
    {
        PTEID_Config::SetTestMode(true);
        sam_server.setString("pki.teste.cartaodecidadao.pt:443");
        settings->setTestMode(true);
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", 
            "Starting App in test mode");
    }

    if (parser->isSet(versionOption)) {
        parser->showVersion();
    }

    if (parser->isSet(helpOption)) {
        parser->showHelp();
    }

    int tGraphicsAccel = settings->getGraphicsAccel();
    if (parser->isSet(hardwareModeOption))
    {
        if (tGraphicsAccel != OPENGL_HARDWARE) {
            settings->setAccelGraphics(OPENGL_HARDWARE);
            PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", 
                "Command line option set OPENGL_HARDWARE. Current option : %d", tGraphicsAccel);
            return RESTART_EXIT_CODE;
        }
    }
    else if (parser->isSet(softwareModeOption))
    {
        if (tGraphicsAccel != OPENGL_SOFTWARE) {
            settings->setAccelGraphics(OPENGL_SOFTWARE);
            PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", 
                "Command line option set OPENGL_SOFTWARE. Current option : %d", tGraphicsAccel);
            return RESTART_EXIT_CODE;
        }
    }
#ifdef WIN32
    else if (parser->isSet(direct3dModeOption))
    {
        qDebug() << "C++:  Command line option to OPENGL_DIRECT3D";
        if (tGraphicsAccel != OPENGL_DIRECT3D) {
            settings->setAccelGraphics(OPENGL_DIRECT3D);
            PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_DEBUG, "eidgui", 
                "Command line option set OPENGL_DIRECT3D. Current option : %d", tGraphicsAccel);
            return RESTART_EXIT_CODE;
        }
    }
#endif
    return SUCCESS_EXIT_CODE;
}

void parseCommandlineGuiArguments(QCommandLineParser *parser, GAPI *gapi){
 
    QStringList args = parser->positionalArguments();
    const QString mode = args.isEmpty() ? QString() : args.first();
    if (mode == "")
    {
        if (!parser->parse(QCoreApplication::arguments())) {
            qDebug() << "ERROR: default no-arguments mode: " << parser->errorText().toStdString().c_str();
            PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_ERROR, "eidgui",
                "parseCommandlineGuiArguments: default no-arguments mode: %s", parser->errorText().toStdString().c_str());
            return;
        }
    }
    // sign is the correct option. signAdvanced and signSimple are legacy options
    else if (mode == "sign" || mode == "signAdvanced" || mode == "signSimple")
    {
        parser->clearPositionalArguments();
        parser->addPositionalArgument(mode, signSimpleDescription );
        QString inputDescription("File  or list of files to be loaded for signing.");
        parser->addPositionalArgument("input", inputDescription);

        const QCommandLineOption timestampingOption("tsa", "Check timestamping");
        const QCommandLineOption reasonOption(QStringList() << "m" << "motivo", "Set default reason", "reason");
        const QCommandLineOption locationOption(QStringList() << "l" << "localidade", "Set default location", "location");

        parser->addOption(timestampingOption);
        parser->addOption(reasonOption);
        parser->addOption(locationOption);

        const QCommandLineOption outputOption(QStringList() << "d" << "destino", "Set output folder", "output");
        parser->addOption(outputOption);

        if (!parser->parse(QCoreApplication::arguments())) {
            qDebug() << "ERROR: " << mode << ": " << parser->errorText().toStdString().c_str();
            PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_ERROR, "eidgui",
                "parseCommandlineGuiArguments: %s: %s", mode.toStdString().c_str(), parser->errorText().toStdString().c_str());
            return;
        }
        // some option values were interpreted as positional args before. call this again
        args = parser->positionalArguments(); 
        // Input Files
        if (args.size() < 2)
        {
            qDebug() << "ERROR: " << mode << ": " << "No input files were provided.";
            PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_ERROR, "eidgui",
                "parseCommandlineGuiArguments: %s: No input files were provided.", mode.toStdString().c_str());
            return;
        }

        for (int i = 1; i < args.size(); i++)
        {
            if (QFileInfo::exists(args[i])) {
                qDebug() << "Adding file: " << args[i];
                gapi->addShortcutPath(args[i]);
            }else
            {
                qDebug() << "ERROR: Cannot load file for signature. File/folder does not exist: " << args[i];
                PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_ERROR, "eidgui",
                    "parseCommandlineGuiArguments: Cannot load file for signature. File/folder does not exist: %s", args[i].toStdString().c_str());
                return;
            }
        }
        // Shortcuts and options
        gapi->setShortcutFlag(GAPI::ShortcutIdSign);
        gapi->setShortcutTsa(parser->isSet(timestampingOption));
        gapi->setShortcutReason(parser->value(reasonOption));
        gapi->setShortcutLocation(parser->value(locationOption));

        // Output dir
        if (parser->isSet(outputOption) && !QFileInfo::exists(parser->value(outputOption)))
        {
            qDebug() << "ERROR: File/folder does not exist for output folder: " << parser->value(outputOption);
            PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_ERROR, "eidgui",
                "parseCommandlineGuiArguments: File/folder does not exist for output folder: %s", parser->value(outputOption).toStdString().c_str());
            gapi->setShortcutFlag(GAPI::ShortcutIdNone);
            return;
        }
        gapi->setShortcutOutput(parser->value(outputOption));
    }
    else
    {
        qDebug() << "ERROR: Unknown mode: " << mode;
        PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_ERROR, "eidgui",
            "parseCommandlineGuiArguments: Unknown mode: %s", mode.toStdString().c_str());
        return;
    }
}

int main(int argc, char *argv[])
{
    int retValue = SUCCESS_EXIT_CODE;

    // GUISettings init
    GUISettings settings;
    AppController::initApplicationScale();

    // AppController init
    AppController controller(settings);

    int tGraphicsAccel = settings.getGraphicsAccel();
    if (tGraphicsAccel == OPENGL_SOFTWARE)
    {
        qDebug() << "C++: Starting App with software graphics acceleration";
        QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
    }
#ifdef WIN32
    else if (tGraphicsAccel == OPENGL_DIRECT3D) {
        qDebug() << "C++: Starting App with ANGLE emulation for OpenGL";
        QCoreApplication::setAttribute(Qt::AA_UseOpenGLES);
    }
#endif
    else
    {
        qDebug() << "C++: Starting App with hardware graphics acceleration";
    }

    PTEID_LOG(eIDMW::PTEID_LOG_LEVEL_CRITICAL, "eidgui", "OpenGL option : %d", tGraphicsAccel);

    SingleApplication app(argc, argv);

    // Parse command line arguments
    QCommandLineParser parser;
    retValue = parseCommandlineAppArguments(&parser, &settings);

    if (retValue == SUCCESS_EXIT_CODE) {

        PTEID_InitSDK();

        app.setOrganizationName("Portuguese State");
        app.setApplicationVersion(settings.getGuiVersion() + " - " + REVISION_NUM_STRING + " [ " + REVISION_HASH_STRING + " ]");

        // Set app icon
        app.setWindowIcon(QIcon(":/appicon.ico"));

        // GAPI init
        GAPI gapi;
        GAPI::declareQMLTypes();

        parseCommandlineGuiArguments(&parser, &gapi);

        QQuickStyle::setStyle("Material");
        //QQuickStyle::setStyle("Universal");
        //QQuickStyle::setStyle("Default");

        QQmlApplicationEngine *engine = new QQmlApplicationEngine();

        // Embedding C++ Objects into QML with Context Properties
        QQmlContext* ctx = engine->rootContext();
        ctx->setContextProperty("gapi", &gapi);
        ctx->setContextProperty("controler", &controller);
        ctx->setContextProperty("image_provider_pdf", gapi.image_provider_pdf);

        engine->addImageProvider("myimageprovider", gapi.buildImageProvider());
        engine->addImageProvider("pdfpreview_imageprovider", gapi.buildPdfImageProvider());

        // Load translation files
        gapi.initTranslation();
        controller.initTranslation();

        // Load main QML file
        engine->load(QUrl(QStringLiteral("qrc:/main.qml")));

        // Each starting instance will make the running instance to restore.
        QObject::connect(
            &app,
            &SingleApplication::instanceStarted,
            &controller,
            &AppController::restoreScreen);

        retValue = app.exec();

        PTEID_ReleaseSDK();
    }

    if (retValue == RESTART_EXIT_CODE) {
        QProcess proc;
        QString cmd;
        for (QString arg : QCoreApplication::arguments()) {
            cmd.append("\"").append(arg).append("\" ");
        }
        cmd = cmd.remove(cmd.length()-1,1); // remove last space
        if (!proc.startDetached(cmd)){
            qDebug() << "Error restarting application: could not start process.";
        }
    }

    return retValue;
}
