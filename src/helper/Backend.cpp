/*
 * Base backend class to be inherited further
 * Copyright (C) 2013 Martin Bříza <mbriza@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include "Backend.h"
#include "HelperApp.h"

#include "backend/PamBackend.h"
#include "backend/PasswdBackend.h"
#include "UserSession.h"

#include <QtCore/QProcessEnvironment>
#include <QtCore/QDebug>

#include <pwd.h>

namespace SDDM {
    Backend::Backend(HelperApp* parent)
            : QObject(parent)
            , m_app(parent) {
    }

    Backend *Backend::get(HelperApp* parent)
    {
    #ifdef USE_PAM
        return new PamBackend(parent);
    #else
        return new PasswdBackend(parent);
    #endif
    }

    void Backend::setAutologin(bool on) {
        m_autologin = on;
    }

    void Backend::setGreeter(bool on) {
        m_greeter = on;
    }

    bool Backend::openSession() {
qDebug() << "Backend::openSession()";
        struct passwd *pw;
        pw = getpwnam(qPrintable(qobject_cast<HelperApp*>(parent())->user()));
        if (pw) {
qDebug() << "pw was found";
            QProcessEnvironment env = m_app->session()->processEnvironment();
            env.insert("HOME", pw->pw_dir);
            env.insert("PWD", pw->pw_dir);
            env.insert("SHELL", pw->pw_shell);
            env.insert("USER", pw->pw_name);
            env.insert("LOGNAME", pw->pw_name);
            if (env.contains("DISPLAY") && !env.contains("XAUTHORITY"))
                env.insert("XAUTHORITY", QString("%1/.Xauthority").arg(pw->pw_dir));
            // TODO: I'm fairly sure this shouldn't be done for PAM sessions, investigate!
qDebug() << "setting ENV data" << env;
            m_app->session()->setProcessEnvironment(env);
        }
qDebug() << "call start()";
        return m_app->session()->start();
    }
}
