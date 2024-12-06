/*
    This source file is part of Konsole, a terminal emulator.

    Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>

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

// Own
#include "ColorSchemeManager.h"

// Qt
#include <QtCore/QIODevice>
#include <QtCore/QFileInfo>
#include <QtCore/QFile>

// KDE
#include <KStandardDirs>
#include <KGlobal>
#include <KConfig>
#include <KLocalizedString>
#include <KDebug>

using namespace Konsole;

ColorSchemeManager::ColorSchemeManager()
    : _haveLoadedAll(false)
{
#if defined(Q_WS_X11)
    // Allow looking up colors in the X11 color database
    QColor::setAllowX11ColorNames(true);
#endif
}

ColorSchemeManager::~ColorSchemeManager()
{
    qDeleteAll(_colorSchemes);
}

K_GLOBAL_STATIC(ColorSchemeManager , theColorSchemeManager)

ColorSchemeManager* ColorSchemeManager::instance()
{
    return theColorSchemeManager;
}

void ColorSchemeManager::loadAllColorSchemes()
{
    int success = 0;
    int failed = 0;

    QStringList nativeColorSchemes = listColorSchemes();
    foreach(const QString& colorScheme, nativeColorSchemes) {
        if (loadColorScheme(colorScheme))
            success++;
        else
            failed++;
    }

    if (failed > 0)
        kWarning() << "failed to load " << failed << " color schemes.";

    _haveLoadedAll = true;
}

QList<const ColorScheme*> ColorSchemeManager::allColorSchemes()
{
    if (!_haveLoadedAll) {
        loadAllColorSchemes();
    }

    return _colorSchemes.values();
}

bool ColorSchemeManager::loadColorScheme(const QString& filePath)
{
    if (!filePath.endsWith(QLatin1String(".colorscheme")) || !QFile::exists(filePath))
        return false;

    QFileInfo info(filePath);

    KConfig config(filePath , KConfig::NoGlobals);
    ColorScheme* scheme = new ColorScheme();
    scheme->setName(info.baseName());
    scheme->read(config);

    if (scheme->name().isEmpty()) {
        kWarning() << "Color scheme in" << filePath << "does not have a valid name and was not loaded.";
        delete scheme;
        return false;
    }

    if (!_colorSchemes.contains(info.baseName())) {
        _colorSchemes.insert(scheme->name(), scheme);
    } else {
        kDebug() << "color scheme with name" << scheme->name() << "has already been" <<
                 "found, ignoring.";

        delete scheme;
    }

    return true;
}

QStringList ColorSchemeManager::listColorSchemes()
{
    return KGlobal::dirs()->findAllResources("data",
            "konsole/*.colorscheme",
            KStandardDirs::NoDuplicates);
}

const ColorScheme ColorSchemeManager::_defaultColorScheme;
const ColorScheme* ColorSchemeManager::defaultColorScheme() const
{
    return &_defaultColorScheme;
}

void ColorSchemeManager::addColorScheme(ColorScheme* scheme)
{
    // remove existing colorscheme with the same name
    if (_colorSchemes.contains(scheme->name())) {
        delete _colorSchemes[scheme->name()];
        _colorSchemes.remove(scheme->name());
    }

    _colorSchemes.insert(scheme->name(), scheme);

    // save changes to disk
    QString path = KGlobal::dirs()->saveLocation("data", "konsole/") + scheme->name() + ".colorscheme";
    KConfig config(path , KConfig::NoGlobals);

    scheme->write(config);
}

bool ColorSchemeManager::deleteColorScheme(const QString& name)
{
    Q_ASSERT(_colorSchemes.contains(name));

    // look up the path and delete
    QString path = findColorSchemePath(name);
    if (QFile::remove(path)) {
        delete _colorSchemes[name];
        _colorSchemes.remove(name);
        return true;
    } else {
        kWarning() << "Failed to remove color scheme -" << path;
        return false;
    }
}

const ColorScheme* ColorSchemeManager::findColorScheme(const QString& name)
{
    if (name.isEmpty())
        return defaultColorScheme();

    // A fix to prevent infinite loops if users puts / in ColorScheme name
    // Konsole will create a sub-folder in that case (bko 315086)
    // More code will have to go in to prevent the users from doing that.
    if (name.contains("/")) {
        kWarning() << name << " has an invalid character / in the name ... skipping";
        return defaultColorScheme();
    }

    if (_colorSchemes.contains(name)) {
        return _colorSchemes[name];
    } else {
        // look for this color scheme
        QString path = findColorSchemePath(name);
        if (!path.isEmpty() && loadColorScheme(path)) {
            return findColorScheme(name);
        }

        kWarning() << "Could not find color scheme - " << name;

        return 0;
    }
}

QString ColorSchemeManager::findColorSchemePath(const QString& name) const
{
    QString path = KStandardDirs::locate("data", "konsole/" + name + ".colorscheme");

    if (!path.isEmpty())
        return path;

    path = KStandardDirs::locate("data", "konsole/" + name + ".schema");

    return path;
}

