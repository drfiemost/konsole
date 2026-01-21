/*
    This file is part of Konsole, an X terminal.

    Copyright 2006-2008 by Robert Knight <robertknight@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA.
*/

// Own
#include "HTMLDecoder.h"

// Qt
#include <KDebug>
#include <QTextStream>

// Konsole
#include "ColorScheme.h"
#include "ExtendedCharTable.h"

using namespace Konsole;

HTMLDecoder::HTMLDecoder() :
    _output(nullptr)
    , _colorTable(ColorScheme::defaultTable)
    , _innerSpanOpen(false)
    , _lastRendition(DEFAULT_RENDITION)
{
}

void HTMLDecoder::begin(QTextStream* output)
{
    _output = output;

    QString text;

    text.append(QStringLiteral("<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\"\n"));
    text.append(QStringLiteral("\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n"));
    text.append(QStringLiteral("<html xmlns=\"http://www.w3.org/1999/xhtml\" lang=\"en\" xml:lang=\"en\">\n"));
    text.append(QStringLiteral("<head>\n"));
    text.append(QStringLiteral("<title>Konsole output</title>\n"));
    text.append(QStringLiteral("<meta http-equiv=\"Content-Type\" content=\"text/html;charset=utf-8\" />\n"));
    text.append(QStringLiteral("</head>\n"));
    text.append(QStringLiteral("<body>\n"));
    text.append(QStringLiteral("<div>\n"));

    //open monospace span
    openSpan(text, QStringLiteral("font-family:monospace"));

    *output << text;
}

void HTMLDecoder::end()
{
    Q_ASSERT(_output);

    QString text;

    closeSpan(text);
    text.append(QStringLiteral("</div>\n"));
    text.append(QStringLiteral("</body>\n"));
    text.append(QStringLiteral("</html>\n"));

    *_output << text;

    _output = nullptr;
}

//TODO: Support for LineProperty (mainly double width , double height)
void HTMLDecoder::decodeLine(const Character* const characters, int count, LineProperty /*properties*/
                            )
{
    Q_ASSERT(_output);

    QString text;

    int spaceCount = 0;

    for (int i = 0; i < count; i++) {
        //check if appearance of character is different from previous char
        if (characters[i].rendition != _lastRendition  ||
                characters[i].foregroundColor != _lastForeColor  ||
                characters[i].backgroundColor != _lastBackColor) {
            if (_innerSpanOpen) {
                closeSpan(text);
                _innerSpanOpen = false;
            }

            _lastRendition = characters[i].rendition;
            _lastForeColor = characters[i].foregroundColor;
            _lastBackColor = characters[i].backgroundColor;

            //build up style string
            QString style;

            //colors - a color table must have been defined first
            if (_colorTable) {
                bool useBold = (_lastRendition & RE_BOLD) != 0;
                if (useBold)
                    style.append(QLatin1String("font-weight:bold;"));

                if ((_lastRendition & RE_UNDERLINE) != 0)
                    style.append(QLatin1String("font-decoration:underline;"));

                style.append(QStringLiteral("color:%1;").arg(_lastForeColor.color(_colorTable).name()));

                style.append(QStringLiteral("background-color:%1;").arg(_lastBackColor.color(_colorTable).name()));
            }

            //open the span with the current style
            openSpan(text, style);
            _innerSpanOpen = true;
        }

        //handle whitespace
        if (characters[i].isSpace())
            spaceCount++;
        else
            spaceCount = 0;

        //output current character
        if (spaceCount < 2) {
            if ((characters[i].rendition & RE_EXTENDED_CHAR) != 0) {
                ushort extendedCharLength = 0;
                const uint* chars = ExtendedCharTable::instance.lookupExtendedChar(characters[i].character, extendedCharLength);
                if (chars) {
                    text.append(QString::fromUcs4(chars, extendedCharLength));
                }
            } else {
                //escape HTML tag characters and just display others as they are
                const QChar ch = characters[i].character;
                if (ch == QLatin1Char('<'))
                    text.append(QLatin1String("&lt;"));
                else if (ch == QLatin1Char('>'))
                    text.append(QLatin1String("&gt;"));
                else if (ch == QLatin1Char('&'))
                    text.append(QLatin1String("&amp;"));
                else
                    text.append(ch);
            }
        } else {
            // HTML truncates multiple spaces, so use a space marker instead
            // Use &#160 instead of &nbsp so xmllint will work.
            text.append(QLatin1String("&#160;"));
        }
    }

    //close any remaining open inner spans
    if (_innerSpanOpen) {
        closeSpan(text);
        _innerSpanOpen = false;
    }

    //start new line
    text.append(QLatin1String("<br>"));

    *_output << text;
}
void HTMLDecoder::openSpan(QString& text , const QString& style)
{
    text.append(QStringLiteral("<span style=\"%1\">").arg(style));
}

void HTMLDecoder::closeSpan(QString& text)
{
    text.append(QStringLiteral("</span>"));
}

void HTMLDecoder::setColorTable(const ColorEntry* table)
{
    _colorTable = table;
}
