/***************************************************************************
* Copyright (c) 2014 Pier Luigi Fiorini <pierluigi.fiorini@gmail.com>
* Copyright (c) 2013 Abdurrahman AVCI <abdurrahmanavci@gmail.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the
* Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
***************************************************************************/

#include "XorgDisplayServer.h"

#include "Configuration.h"
#include "DaemonApp.h"
#include "Display.h"
#include "SignalHandler.h"

#include <QDebug>
#include <QFile>
#include <QProcess>
#include <QUuid>

#include <random>

#include <xcb/xcb.h>

#include <pwd.h>
#include <unistd.h>

namespace SDDM {
    XorgDisplayServer::XorgDisplayServer(Display *parent) : DisplayServer(parent) {
        // get auth directory
        QString authDir = RUNTIME_DIR;

        // use "." as authdir in test mode
        if (daemonApp->testing())
            authDir = QStringLiteral(".");

        // create auth dir if not existing
        QDir().mkpath(authDir);

        // set auth path
        m_authPath = QString("%1/%2").arg(authDir).arg(QUuid::createUuid().toString());

        // generate cookie
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);

        // resever 32 bytes
        m_cookie.reserve(32);

        // create a random hexadecimal number
        const char *digits = "0123456789abcdef";
        for (int i = 0; i < 32; ++i)
            m_cookie[i] = digits[dis(gen)];
    }

    XorgDisplayServer::~XorgDisplayServer() {
        stop();
    }

    const QString &XorgDisplayServer::display() const {
        return m_display;
    }

    const QString &XorgDisplayServer::authPath() const {
        return m_authPath;
    }

    QString XorgDisplayServer::sessionType() const {
        return QStringLiteral("x11");
    }

    const QString &XorgDisplayServer::cookie() const {
        return m_cookie;
    }

    void XorgDisplayServer::addCookie(const QString &file) {
        // log message
        qDebug() << "Adding cookie to" << file;

        // Touch file
        QFile file_handler(file);
        file_handler.open(QIODevice::WriteOnly);
        file_handler.close();

        QString cmd = QString("%1 -f %2 -q").arg(mainConfig.XDisplay.XauthPath.get()).arg(file);

        // execute xauth
        FILE *fp = popen(qPrintable(cmd), "w");

        // check file
        if (!fp)
            return;
        fprintf(fp, "remove %s\n", qPrintable(m_display));
        fprintf(fp, "add %s . %s\n", qPrintable(m_display), qPrintable(m_cookie));
        fprintf(fp, "exit\n");

        // close pipe
        pclose(fp);
    }

    bool XorgDisplayServer::start() {
        // check flag
        if (m_started)
            return false;

        // generate auth file
        addCookie(m_authPath);
        changeOwner(m_authPath);

        // create process
        process = new QProcess(this);

        // delete process on finish
        connect(process, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(finished()));

        // log message
        qDebug() << "Display server starting...";

        if (daemonApp->testing()) {
            QStringList args;
            args << m_display << "-ac" << "-br" << "-noreset" << "-screen" << "800x600";
            process->start(mainConfig.XDisplay.XephyrPath.get(), args);


            // wait for display server to start
            if (!process->waitForStarted()) {
                // log message
                qCritical() << "Failed to start display server process.";

                // return fail
                return false;
            }
            emit started();
        } else {
            // set process environment
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("XCURSOR_THEME", mainConfig.Theme.CursorTheme.get());
            process->setProcessEnvironment(env);

            //create pipe for communicating with X server
            //0 == read from X, 1== write to from X
            int pipeFds[2];
            if (pipe(pipeFds) != 0) {
                qCritical("Could not create pipe to start X server");
            }

            // start display server
            QStringList args;
            args << "-auth" << m_authPath
                 << "-nolisten" << "tcp"
                 << "-background" << "none"
                 << "-noreset"
                 << "-displayfd" << QString::number(pipeFds[1])
                 << QString("vt%1").arg(displayPtr()->terminalId());
            qDebug() << "Running:"
                     << qPrintable(mainConfig.XDisplay.ServerPath.get())
                     << qPrintable(args.join(" "));
            process->start(mainConfig.XDisplay.ServerPath.get(), args);

            // wait for display server to start
            if (!process->waitForStarted()) {
                // log message
                qCritical() << "Failed to start display server process.";

                // return fail
                close(pipeFds[0]);
                return false;
            }

            QFile readPipe;

            if (!readPipe.open(pipeFds[0], QIODevice::ReadOnly)) {
                qCritical("Failed to open pipe to start X Server ");

                close(pipeFds[0]);
                return false;
            }
            QByteArray displayNumber = readPipe.readLine();
            displayNumber.prepend(QByteArray(":"));
            displayNumber.remove(displayNumber.size() -1, 1); //trim trailing whitespace
            m_display= displayNumber;
    
            // close our pipe
            close(pipeFds[0]);

            emit started();
        }

        // set flag
        m_started = true;

        // return success
        return true;
    }

    void XorgDisplayServer::stop() {
        // check flag
        if (!m_started)
            return;

        // log message
        qDebug() << "Display server stopping...";

        // terminate process
        process->terminate();

        // wait for finished
        if (!process->waitForFinished(5000))
            process->kill();
    }

    void XorgDisplayServer::finished() {
        // check flag
        if (!m_started)
            return;

        // reset flag
        m_started = false;

        // log message
        qDebug() << "Display server stopped.";

        // clean up
        process->deleteLater();
        process = nullptr;

        // remove authority file
        QFile::remove(m_authPath);

        // emit signal
        emit stopped();
    }

    void XorgDisplayServer::setupDisplay() {
        QString displayCommand = mainConfig.XDisplay.DisplayCommand.get();

        // create display setup script process
        QProcess *displayScript = new QProcess();

        // set process environment
        QProcessEnvironment env;
        env.insert("DISPLAY", m_display);
        env.insert("HOME", "/");
        env.insert("PATH", mainConfig.Users.DefaultPath.get());
        env.insert("XAUTHORITY", m_authPath);
        env.insert("SHELL", "/bin/sh");
        displayScript->setProcessEnvironment(env);

        // start display setup script
        qDebug() << "Running display setup script " << displayCommand;
        displayScript->start(displayCommand);
    }

    void XorgDisplayServer::changeOwner(const QString &fileName) {
        // change the owner and group of the auth file to the sddm user
        // TODO: sddm user is supposed to be configurable
        struct passwd *pw = getpwnam("sddm");
        if (!pw)
            qWarning() << "Failed to find the sddm user. Owner of the auth file will not be changed.";
        else {
            if (chown(qPrintable(fileName), pw->pw_uid, pw->pw_gid) == -1)
                qWarning() << "Failed to change owner of the auth file.";
        }
    }
}
