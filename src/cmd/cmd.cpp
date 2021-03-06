/*
 * Copyright (C) by Olivier Goffart <ogoffart@owncloud.com>
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <iostream>
#include <qcoreapplication.h>
#include <QStringList>
#include <QUrl>
#include <QFile>
#include <qdebug.h>

#include <neon/ne_socket.h>

#include "account.h"
#include "clientproxy.h"
#include "creds/httpcredentials.h"
#include "csync.h"
#include "simplesslerrorhandler.h"
#include "syncengine.h"
#include "syncjournaldb.h"
#include "config.h"

#include "cmd.h"

#include "netrcparser.h"

#ifdef Q_OS_WIN32
#include <windows.h>
#else
#include <termios.h>
#endif

using namespace Mirall;

struct CmdOptions {
    QString source_dir;
    QString target_url;
    QString config_directory;
    QString user;
    QString password;
    QString proxy;
    bool silent;
    bool trustSSL;
    bool useNetrc;
    bool interactive;
    QString exclude;
};

// we can't use csync_set_userdata because the SyncEngine sets it already.
// So we have to use a global variable
CmdOptions *opts = 0;

class EchoDisabler
{
public:
    EchoDisabler()
    {
#ifdef Q_OS_WIN
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        GetConsoleMode(hStdin, &mode);
        SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));
#else
        tcgetattr(STDIN_FILENO, &tios);
        termios tios_new = tios;
        tios_new.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &tios_new);
#endif
    }

    ~EchoDisabler()
    {
#ifdef Q_OS_WIN
        SetConsoleMode(hStdin, mode);
#else
        tcsetattr(STDIN_FILENO, TCSANOW, &tios);
#endif
    }
private:
#ifdef Q_OS_WIN
    DWORD mode = 0;
#else
    termios tios;
#endif
};

QString queryPassword(const QString &user)
{
    EchoDisabler disabler;
    std::cout << "Password for user " << qPrintable(user) << ": ";
    std::string s;
    std::getline(std::cin, s);
    return QString::fromStdString(s);
}

class HttpCredentialsText : public HttpCredentials {
public:
    HttpCredentialsText(const QString& user, const QString& password) : HttpCredentials(user, password) {}
    QString queryPassword(bool *ok) {
        if (ok) {
            *ok = true;
        }
        return ::queryPassword(user());
    }
};

void help()
{
    const char* appName = APPLICATION_EXECUTABLE "cmd";
    std::cout << appName << " - command line " APPLICATION_NAME " client tool." << std::endl;
    std::cout << "" << std::endl;
    std::cout << "Usage: " << appName << " <source_dir> <server_url>" << std::endl;
    std::cout << "" << std::endl;
    std::cout << "A proxy can either be set manually using --httpproxy." << std::endl;
    std::cout << "Otherwise, the setting from a configured sync client will be used." << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --silent, -s           Don't be so verbose" << std::endl;
    std::cout << "  --httpproxy [proxy]    Specify a http proxy to use." << std::endl;
    std::cout << "                         Proxy is http://server:port" << std::endl;
    std::cout << "  --trust                Trust the SSL certification." << std::endl;
    std::cout << "  --exclude [file]       exclude list file" << std::endl;
    std::cout << "  --user, -u [name]      Use [name] as the login name" << std::endl;
    std::cout << "  --password, -p [pass]  Use [pass] as password" << std::endl;
    std::cout << "  -n                     Use netrc (5) for login" << std::endl;
    std::cout << "  --non-interactive      Do not block execution with interaction" << std::endl;
    std::cout << "" << std::endl;
    exit(1);

}

void parseOptions( const QStringList& app_args, CmdOptions *options )
{
    QStringList args(app_args);

    if( args.count() < 3 ) {
        help();
    }

    options->target_url = args.takeLast();
    // check if the remote.php/webdav tail was added and append if not.
    if( !options->target_url.contains("remote.php/webdav")) {
        if(!options->target_url.endsWith("/")) {
            options->target_url.append("/");
        }
        options->target_url.append("remote.php/webdav");
    }
    if (options->target_url.startsWith("http"))
        options->target_url.replace(0, 4, "owncloud");
    options->source_dir = args.takeLast();
    if( !QFile::exists( options->source_dir )) {
        std::cerr << "Source dir does not exists." << std::endl;
        exit(1);
    }

    QStringListIterator it(args);
    // skip file name;
    if (it.hasNext()) it.next();

    while(it.hasNext()) {
        const QString option = it.next();

        if( option == "--httpproxy" && !it.peekNext().startsWith("-")) {
            options->proxy = it.next();
        } else if( option == "-s" || option == "--silent") {
            options->silent = true;
        } else if( option == "--trust") {
            options->trustSSL = true;
        } else if( option == "-n") {
            options->useNetrc = true;
        } else if( option == "--non-interactive") {
            options->interactive = false;
        } else if( (option == "-u" || option == "--user") && !it.peekNext().startsWith("-") ) {
                options->user = it.next();
        } else if( (option == "-p" || option == "--password") && !it.peekNext().startsWith("-") ) {
                options->user = it.next();
        } else if( option == "--exclude" && !it.peekNext().startsWith("-") ) {
                options->exclude = it.next();
        } else {
            help();
        }
    }

    if( options->target_url.isEmpty() || options->source_dir.isEmpty() ) {
        help();
    }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    CmdOptions options;
    options.silent = false;
    options.trustSSL = false;
    options.useNetrc = false;
    options.interactive = true;
    ClientProxy clientProxy;

    parseOptions( app.arguments(), &options );


    QUrl url = QUrl::fromUserInput(options.target_url);    

    // Fetch username and password. If empty, try to retrieve
    // from URL and strip URL
    QString user;
    QString password;

    if (options.useNetrc) {
        NetrcParser parser;
        if (parser.parse()) {
            NetrcParser::LoginPair pair = parser.find(url.host());
            user = pair.first;
            password = pair.second;
        }
    } else {
        user = options.user;
        if (user.isEmpty()) {
            user = url.userName();
        }
        password = options.password;
        if (password.isEmpty()) {
            password = url.password();
        }

        if (options.interactive) {
            if (user.isEmpty()) {
                std::cout << "Please enter user name: ";
                std::string s;
                std::getline(std::cin, s);
                user = QString::fromStdString(s);
            }
            if (password.isEmpty()) {
                password = queryPassword(user);
            }
        }
    }

    // ### ensure URL is free of credentials
    if (url.userName().isEmpty()) {
        url.setUserName(user);
    }
    if (url.password().isEmpty()) {
        url.setPassword(password);
    }

    Account account;

    // Find the folder and the original owncloud url
    QStringList splitted = url.path().split(account.davPath());
    url.setPath(splitted.value(0));
    url.setScheme(url.scheme().replace("owncloud", "http"));
    QString folder = splitted.value(1);

    SimpleSslErrorHandler *sslErrorHandler = new SimpleSslErrorHandler;

    HttpCredentials *cred = new HttpCredentialsText(user, password);

    account.setUrl(url);
    account.setCredentials(cred);
    account.setSslErrorHandler(sslErrorHandler);

    AccountManager::instance()->setAccount(&account);

restart_sync:

    CSYNC *_csync_ctx;
    if( csync_create( &_csync_ctx, options.source_dir.toUtf8(),
                      url.toEncoded().constData()) < 0 ) {
        qFatal("Unable to create csync-context!");
        return EXIT_FAILURE;
    }
    int rc = ne_sock_init();
    if (rc < 0) {
        qFatal("ne_sock_init failed!");
    }

    csync_set_log_level(options.silent ? 1 : 11);

    opts = &options;
    cred->syncContextPreInit(_csync_ctx);

    if( csync_init( _csync_ctx ) < 0 ) {
        qFatal("Could not initialize csync!");
        return EXIT_FAILURE;
    }

    csync_set_module_property(_csync_ctx, "csync_context", _csync_ctx);
    if( !options.proxy.isNull() ) {
        QString host;
        int port = 0;
        bool ok;

        // Set as default and let overwrite later
        csync_set_module_property(_csync_ctx, "proxy_type", (void*) "NoProxy");

        QStringList pList = options.proxy.split(':');
        if(pList.count() == 3) {
            // http: //192.168.178.23 : 8080
            //  0            1            2
            host = pList.at(1);
            if( host.startsWith("//") ) host.remove(0, 2);

            port = pList.at(2).toInt(&ok);

            if( !host.isNull() ) {
                csync_set_module_property(_csync_ctx, "proxy_type", (void*) "HttpProxy");
                csync_set_module_property(_csync_ctx, "proxy_host", host.toUtf8().data());
                if( ok && port ) {
                    csync_set_module_property(_csync_ctx, "proxy_port", (void*) &port);
                }
            }
        }
    } else {
        clientProxy.setupQtProxyFromConfig();
        QString url( options.target_url );
        if( url.startsWith("owncloud")) {
            url.remove(0, 8);
            url = QString("http%1").arg(url);
        }
        clientProxy.setCSyncProxy(QUrl(url), _csync_ctx);
    }

    if (!options.exclude.isEmpty()) {
        csync_add_exclude_list(_csync_ctx, options.exclude.toLocal8Bit());
    }

    cred->syncContextPreStart(_csync_ctx);

    Cmd cmd;
    SyncJournalDb db(options.source_dir);
    SyncEngine engine(_csync_ctx, options.source_dir, QUrl(options.target_url).path(), folder, &db);
    QObject::connect(&engine, SIGNAL(finished()), &app, SLOT(quit()));
    QObject::connect(&engine, SIGNAL(transmissionProgress(Progress::Info)), &cmd, SLOT(transmissionProgressSlot()));

    // Have to be done async, else, an error before exec() does not terminate the event loop.
    QMetaObject::invokeMethod(&engine, "startSync", Qt::QueuedConnection);

    app.exec();

    csync_destroy(_csync_ctx);

    ne_sock_exit();

    if (engine.isAnotherSyncNeeded()) {
        qDebug() << "Restarting Sync, because another sync is needed";
        goto restart_sync;
    }

    return 0;
}

