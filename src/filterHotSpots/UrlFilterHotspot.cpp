/*
    Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>
    Copyright 2020 by Tomaz Canabrava <tcanabrava@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA.
*/

#include "UrlFilterHotspot.h"


#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QRegExp>

#include <QIcon>
#include <KLocalizedString>
#include <KRun>

#include "UrlFilter.h"
//regexp matches:
// full url:

using namespace Konsole;


UrlFilterHotSpot::UrlFilterHotSpot(int startLine, int startColumn, int endLine, int endColumn,
            const QStringList &capturedTexts)
    : RegExpFilterHotSpot(startLine, startColumn, endLine, endColumn, capturedTexts)
{
    setType(Link);
}

UrlFilterHotSpot::UrlType UrlFilterHotSpot::urlType() const
{
    const QString url = capturedTexts().at(0);

    if (UrlFilter::FullUrlRegExp.exactMatch(url))
        return StandardUrl;
    else if (UrlFilter::EmailAddressRegExp.exactMatch(url))
        return Email;
    else
        return Unknown;
}

void UrlFilterHotSpot::activate(QObject* object)
{
    QString url = capturedTexts().at(0);

    const UrlType kind = urlType();

    const QString& actionName = object ? object->objectName() : QString();

    if (actionName == QLatin1String("copy-action")) {
        QApplication::clipboard()->setText(url);
        return;
    }

    if (!object || actionName == QLatin1String("open-action")) {
        if (kind == StandardUrl) {
            // if the URL path does not include the protocol ( eg. "www.kde.org" ) then
            // prepend http:// ( eg. "www.kde.org" --> "http://www.kde.org" )
            if (!url.contains(QLatin1String("://"))) {
                url.prepend(QLatin1String("http://"));
            }
        } else if (kind == Email) {
            url.prepend(QLatin1String("mailto:"));
        }

        new KRun(url, QApplication::activeWindow());
    }
}

UrlFilterHotSpot::~UrlFilterHotSpot()
{
}

QList<QAction*> UrlFilterHotSpot::actions()
{
    QAction* openAction = new QAction(this);
    QAction* copyAction = new QAction(this);

    const UrlType kind = urlType();
    Q_ASSERT(kind == StandardUrl || kind == Email);

    if (kind == StandardUrl) {
        openAction->setText(i18n("Open Link"));
        openAction->setIcon(QIcon::fromTheme(QStringLiteral("internet-services")));
        copyAction->setText(i18n("Copy Link Address"));
        copyAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-copy-url")));
    } else if (kind == Email) {
        openAction->setText(i18n("Send Email To..."));
        openAction->setIcon(QIcon::fromTheme(QStringLiteral("mail-send")));
        copyAction->setText(i18n("Copy Email Address"));
        copyAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-copy-mail")));
    }

    // object names are set here so that the hotspot performs the
    // correct action when activated() is called with the triggered
    // action passed as a parameter.
    openAction->setObjectName(QLatin1String("open-action"));
    copyAction->setObjectName(QLatin1String("copy-action"));

    QObject::connect(openAction, &QAction::triggered, this, [this, openAction]{ activate(openAction); });
    QObject::connect(copyAction, &QAction::triggered, this, [this, copyAction]{ activate(copyAction); });

    QList<QAction*> actions;
    actions << openAction;
    actions << copyAction;

    return actions;
}
