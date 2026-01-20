/*
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
#include "Filter.h"

#include "HotSpot.h"

// Qt
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QString>
#include <QTextStream>

// KDE
#include <KLocalizedString>
#include <KRun>

// Konsole
#include "TerminalCharacterDecoder.h"

using namespace Konsole;

Filter::Filter()
    : _hotspots(QMultiHash<int, HotSpot*>())
    , _hotspotList(QList<HotSpot*>())
    , _linePositions(nullptr)
    , _buffer(nullptr)
{
}

Filter::~Filter()
{
    reset();
}
void Filter::reset()
{
    _hotspots.clear();
    QListIterator<HotSpot *> iter(_hotspotList);
    while (iter.hasNext()) {
        delete iter.next();
    }
    _hotspotList.clear();
}

void Filter::setBuffer(const QString* buffer , const QList<int>* linePositions)
{
    _buffer = buffer;
    _linePositions = linePositions;
}

std::pair<int, int> Filter::getLineColumn(int position)
{
    Q_ASSERT(_linePositions);
    Q_ASSERT(_buffer);

    for (int i = 0 ; i < _linePositions->count() ; i++) {

        const int nextLine = i == _linePositions->count() - 1
            ? _buffer->length() + 1
            : _linePositions->value(i + 1);

        if (_linePositions->value(i) <= position && position < nextLine) {
            return std::make_pair(i,  Character::stringWidth(buffer()->mid(_linePositions->value(i),
                                                     position - _linePositions->value(i))));
        }
    }
    return std::make_pair(-1, -1);
}

const QString* Filter::buffer()
{
    return _buffer;
}
void Filter::addHotSpot(HotSpot* spot)
{
    _hotspotList << spot;

    for (int line = spot->startLine() ; line <= spot->endLine() ; line++) {
        _hotspots.insert(line, spot);
    }
}
QList<HotSpot*> Filter::hotSpots() const
{
    return _hotspotList;
}

HotSpot* Filter::hotSpotAt(int line , int column) const
{
    const QList<HotSpot*> hotspots = _hotspots.values(line);

    for (HotSpot *spot: hotspots) {
        if (spot->startLine() == line && spot->startColumn() > column)
            continue;
        if (spot->endLine() == line && spot->endColumn() < column)
            continue;

        return spot;
    }

    return nullptr;
}

#include "Filter.moc"
