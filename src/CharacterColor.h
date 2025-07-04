/*
    This file is part of Konsole, KDE's terminal.

    Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>
    Copyright 1997,1998 by Lars Doelle <lars.doelle@on-line.de>

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

#ifndef CHARACTERCOLOR_H
#define CHARACTERCOLOR_H

// Qt
#include <QtGui/QColor>

namespace Konsole
{
/**
 * An entry in a terminal display's color palette.
 *
 * A color palette is an array of 16 ColorEntry instances which map
 * system color indexes (from 0 to 15) into actual colors.
 *
 * Each entry can be set as bold, in which case any text
 * drawn using the color should be drawn in bold.
 */
class ColorEntry
{
public:
    /** Specifies the weight to use when drawing text with this color. */
    enum FontWeight {
        /** Always draw text in this color with a bold weight. */
        Bold,
        /** Always draw text in this color with a normal weight. */
        Normal,
        /**
         * Use the current font weight set by the terminal application.
         * This is the default behavior.
         */
        UseCurrentFormat
    };

    /**
     * Constructs a new color palette entry.
     *
     * @param c The color value for this entry.
     * @param weight Specifies the font weight to use when drawing text with this color.
     */
    explicit ColorEntry(QColor c,  FontWeight weight = UseCurrentFormat)
        : color(c), fontWeight(weight) {}

    /**
     * Constructs a new color palette entry with an undefined color, and
     * with the bold flags set to false.
     */
    ColorEntry() : fontWeight(UseCurrentFormat) {}

    ColorEntry(const ColorEntry& rhs)
        : color(rhs.color), fontWeight(rhs.fontWeight) {}

    /**
     * Sets the color and boldness of this color to those of @p rhs.
     */
    ColorEntry& operator=(const ColorEntry& rhs) = default;

    /** The color value of this entry for display. */
    QColor color;

    /**
     * Specifies the font weight to use when drawing text with this color.
     * This is not applicable when the color is used to draw a character's background.
     */
    FontWeight fontWeight;

    /**
     * Compares two color entries and returns true if they represent the same
     * color and font weight.
     */
    friend bool operator == (const ColorEntry& a, const ColorEntry& b);
    /**
     * Compares two color entries and returns true if they represent different
     * color and font weight.
     */
    friend bool operator != (const ColorEntry& a, const ColorEntry& b);
};

inline bool operator == (const ColorEntry& a, const ColorEntry& b)
{
    return  a.color == b.color &&
            a.fontWeight == b.fontWeight;
}

inline bool operator != (const ColorEntry& a, const ColorEntry& b)
{
    return !operator==(a, b);
}

// Attributed Character Representations ///////////////////////////////

// Colors

constexpr int BASE_COLORS  = (2+8);
constexpr int INTENSITIES  = 2;
constexpr int TABLE_COLORS = (INTENSITIES*BASE_COLORS);

constexpr int DEFAULT_FORE_COLOR = 0;
constexpr int DEFAULT_BACK_COLOR = 1;

/* CharacterColor is a union of the various color spaces.

   Assignment is as follows:

   Type  - Space        - Values

   0     - Undefined   - u:  0,      v:0        w:0
   1     - Default     - u:  0..1    v:intense  w:0
   2     - System      - u:  0..7    v:intense  w:0
   3     - Index(256)  - u: 16..255  v:0        w:0
   4     - RGB         - u:  0..255  v:0..256   w:0..256

   Default color space has two separate colors, namely
   default foreground and default background color.
*/

constexpr quint8 COLOR_SPACE_UNDEFINED  = 0;
constexpr quint8 COLOR_SPACE_DEFAULT    = 1;
constexpr quint8 COLOR_SPACE_SYSTEM     = 2;
constexpr quint8 COLOR_SPACE_256        = 3;
constexpr quint8 COLOR_SPACE_RGB        = 4;

/**
 * Describes the color of a single character in the terminal.
 */
class CharacterColor
{
    friend class Character;

public:
    /** Constructs a new CharacterColor whose color and color space are undefined. */
    CharacterColor()
        : _colorSpace(COLOR_SPACE_UNDEFINED),
          _u(0),
          _v(0),
          _w(0)
    {}

    /**
     * Constructs a new CharacterColor using the specified @p colorSpace and with
     * color value @p co
     *
     * The meaning of @p co depends on the @p colorSpace used.
     *
     * TODO : Document how @p co relates to @p colorSpace
     *
     * TODO : Add documentation about available color spaces.
     */
    CharacterColor(quint8 colorSpace, int co)
        : _colorSpace(colorSpace),
          _u(0),
          _v(0),
          _w(0) {
        switch (colorSpace) {
        case COLOR_SPACE_DEFAULT:
            _u = co & 1;
            break;
        case COLOR_SPACE_SYSTEM:
            _u = co & 7;
            _v = (co >> 3) & 1;
            break;
        case COLOR_SPACE_256:
            _u = co & 255;
            break;
        case COLOR_SPACE_RGB:
            _u = co >> 16;
            _v = co >> 8;
            _w = co;
            break;
        default:
            _colorSpace = COLOR_SPACE_UNDEFINED;
        }
    }

    /**
     * Returns true if this character color entry is valid.
     */
    bool isValid() const {
        return _colorSpace != COLOR_SPACE_UNDEFINED;
    }

    /**
     * Set this color as an intensive system color.
     *
     * This is only applicable if the color is using the COLOR_SPACE_DEFAULT or COLOR_SPACE_SYSTEM
     * color spaces.
     */
    void setIntensive();

    /**
     * Returns the color within the specified color @p palette
     *
     * The @p palette is only used if this color is one of the 16 system colors, otherwise
     * it is ignored.
     */
    QColor color(const ColorEntry* palette) const;

    /**
     * Compares two colors and returns true if they represent the same color value and
     * use the same color space.
     */
    friend bool operator == (const CharacterColor& a, const CharacterColor& b);
    /**
     * Compares two colors and returns true if they represent different color values
     * or use different color spaces.
     */
    friend bool operator != (const CharacterColor& a, const CharacterColor& b);

private:
    quint8 _colorSpace;

    // bytes storing the character color
    quint8 _u;
    quint8 _v;
    quint8 _w;
};

inline bool operator == (const CharacterColor& a, const CharacterColor& b)
{
    return     a._colorSpace == b._colorSpace &&
               a._u == b._u &&
               a._v == b._v &&
               a._w == b._w;
}
inline bool operator != (const CharacterColor& a, const CharacterColor& b)
{
    return !operator==(a, b);
}

inline const QColor color256(quint8 u, const ColorEntry* base)
{
    //   0.. 16: system colors
    if (u < 8) {
        return base[u + 2].color;
    }
    u -= 8;
    if (u < 8) {
        return base[u + 2 + BASE_COLORS].color;
    }
    u -= 8;

    //  16..231: 6x6x6 rgb color cube
    if (u < 216) {
        return QColor(((u / 36) % 6) ? (40 * ((u / 36) % 6) + 55) : 0,
                      ((u / 6) % 6) ? (40 * ((u / 6) % 6) + 55) : 0,
                      ((u / 1) % 6) ? (40 * ((u / 1) % 6) + 55) : 0);
    }
    u -= 216;

    // 232..255: gray, leaving out black and white
    int gray = u * 10 + 8;

    return QColor(gray, gray, gray);
}

inline QColor CharacterColor::color(const ColorEntry* base) const
{
    switch (_colorSpace) {
    case COLOR_SPACE_DEFAULT:
        return base[_u + 0 + (_v ? BASE_COLORS : 0)].color;
    case COLOR_SPACE_SYSTEM:
        return base[_u + 2 + (_v ? BASE_COLORS : 0)].color;
    case COLOR_SPACE_256:
        return color256(_u, base);
    case COLOR_SPACE_RGB:
        return QColor(_u, _v, _w);
    case COLOR_SPACE_UNDEFINED:
        return QColor();
    }

    Q_ASSERT(false); // invalid color space

    return QColor();
}

inline void CharacterColor::setIntensive()
{
    if (_colorSpace == COLOR_SPACE_SYSTEM || _colorSpace == COLOR_SPACE_DEFAULT) {
        _v = 1;
    }
}
}

#endif // CHARACTERCOLOR_H

