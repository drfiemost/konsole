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

#include "RegExpFilter.h"

#include "RegExpFilterHotspot.h"

using namespace Konsole;

RegExpFilter::RegExpFilter()
    : _searchText(QRegExp())
{
}

void RegExpFilter::setRegExp(const QRegExp& regExp)
{
    _searchText = regExp;
}

QRegExp RegExpFilter::regExp() const
{
    return _searchText;
}

void RegExpFilter::process()
{
    int pos = 0;
    const QString* text = buffer();

    Q_ASSERT(text);

    // ignore any regular expressions which match an empty string.
    // otherwise the while loop below will run indefinitely
    if (_searchText.isEmpty() || _searchText.pattern().isEmpty())
        return;

    while (pos >= 0) {
        pos = _searchText.indexIn(*text, pos);

        if (pos >= 0) {
            auto [startLine, startColumn] = getLineColumn(pos);
            auto [endLine, endColumn] = getLineColumn(pos + _searchText.matchedLength());

            HotSpot* spot = newHotSpot(startLine, startColumn,
                                       endLine, endColumn,
                                       _searchText.capturedTexts());

            addHotSpot(spot);
            pos += _searchText.matchedLength();

            // if matchedLength == 0, the program will get stuck in an infinite loop
            if (_searchText.matchedLength() == 0)
                pos = -1;
        }
    }
}

HotSpot* RegExpFilter::newHotSpot(int startLine, int startColumn,
        int endLine, int endColumn, const QStringList &capturedTexts)
{
    return new RegExpFilterHotSpot(startLine, startColumn,
                                   endLine, endColumn, capturedTexts);
}
