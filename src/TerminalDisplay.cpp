/*
    This file is part of Konsole, a terminal emulator for KDE.

    Copyright 2006-2008 by Robert Knight <robertknight@gmail.com>
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

// Own
#include "TerminalDisplay.h"

// Config
#include <config-konsole.h>

// Qt
#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QAction>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QToolTip>
#include <QAccessible>

// KDE
#include <KShell>
#include <KColorScheme>
#include <KCursor>
#include <KDebug>
#include <KLocalizedString>
#include <KNotification>
#include <KGlobalSettings>
#include <KIO/NetAccess>
#if defined(HAVE_LIBKONQ)
    #include <konq_operations.h>
#endif

#include <KFileItem>
#include <KMessageBox>

// Konsole
#include "Filter.h"
#include "TerminalCharacterDecoder.h"
#include "Screen.h"
#include "LineFont.h"
#include "SessionController.h"
#include "ExtendedCharTable.h"
#include "TerminalDisplayAccessible.h"
#include "SessionManager.h"
#include "Session.h"
#include "Profile.h"
#include "ViewManager.h" // for colorSchemeForProfile. // TODO: Rewrite this.
#include "IncrementalSearchBar.h"

#define MAX_LINE_WIDTH 1024

using namespace Konsole;

#define REPCHAR   "ABCDEFGHIJKLMNOPQRSTUVWXYZ" \
    "abcdefgjijklmnopqrstuvwxyz" \
    "0123456789./+@"

// we use this to force QPainter to display text in LTR mode
// more information can be found in: http://unicode.org/reports/tr9/
const QChar LTR_OVERRIDE_CHAR(0x202D);

inline int TerminalDisplay::loc(int x, int y) const {
    Q_ASSERT(_lines > 0);
    Q_ASSERT(_columns > 0);
    Q_ASSERT(y >= 0 && y < _lines);
    Q_ASSERT(x >= 0 && x < _columns);
    x = std::clamp(x, 0, _columns - 1);
    y = std::clamp(y, 0, _lines - 1);

    return y * _columns + x;
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                Colors                                     */
/*                                                                           */
/* ------------------------------------------------------------------------- */

/* Note that we use ANSI color order (bgr), while IBMPC color order is (rgb)

   Code        0       1       2       3       4       5       6       7
   ----------- ------- ------- ------- ------- ------- ------- ------- -------
   ANSI  (bgr) Black   Red     Green   Yellow  Blue    Magenta Cyan    White
   IBMPC (rgb) Black   Blue    Green   Cyan    Red     Magenta Yellow  White
*/

ScreenWindow* TerminalDisplay::screenWindow() const
{
    return _screenWindow;
}
void TerminalDisplay::setScreenWindow(ScreenWindow* window)
{
    // disconnect existing screen window if any
    if (!_screenWindow.isNull()) {
        disconnect(_screenWindow , nullptr , this , nullptr);
    }

    _screenWindow = window;

    if (!_screenWindow.isNull()) {
        connect(_screenWindow.data() , &Konsole::ScreenWindow::outputChanged , this , &Konsole::TerminalDisplay::updateLineProperties);
        connect(_screenWindow.data() , &Konsole::ScreenWindow::outputChanged , this , &Konsole::TerminalDisplay::updateImage);
        connect(_screenWindow.data() , &Konsole::ScreenWindow::currentResultLineChanged , this , &Konsole::TerminalDisplay::updateImage);
        connect(_screenWindow, &Konsole::ScreenWindow::outputChanged, this, [this]() {
            _filterUpdateRequired = true;
        });
        connect(_screenWindow, &Konsole::ScreenWindow::scrolled, this, [this](int) {
            _filterUpdateRequired = true;
        });
        _screenWindow->setWindowLines(_lines);
    }
}

const ColorEntry* TerminalDisplay::colorTable() const
{
    return _colorTable;
}
void TerminalDisplay::setBackgroundColor(const QColor& color)
{
    _colorTable[DEFAULT_BACK_COLOR] = color;

    QPalette p = palette();
    p.setColor(backgroundRole(), color);
    setPalette(p);

    // Avoid propagating the palette change to the scroll bar
    _scrollBar->setPalette(QApplication::palette());

    update();
}
QColor TerminalDisplay::getBackgroundColor() const
{
    QPalette p = palette();
    return p.color(backgroundRole());
}
void TerminalDisplay::setForegroundColor(const QColor& color)
{
    _colorTable[DEFAULT_FORE_COLOR] = color;

    update();
}
void TerminalDisplay::setColorTable(const ColorEntry table[])
{
    for (int i = 0; i < TABLE_COLORS; i++)
        _colorTable[i] = table[i];

    setBackgroundColor(_colorTable[DEFAULT_BACK_COLOR]);
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                   Font                                    */
/*                                                                           */
/* ------------------------------------------------------------------------- */

static inline bool isLineCharString(const QString& string)
{
    if (string.length() == 0)
        return false;

    return isSupportedLineChar(string.at(0).unicode());
}

void TerminalDisplay::fontChange(const QFont&)
{
    QFontMetrics fm(font());
    _fontHeight = fm.height() + _lineSpacing;

    Q_ASSERT(_fontHeight > 0);

    // waba TerminalDisplay 1.123:
    // "Base character width on widest ASCII character. This prevents too wide
    //  characters in the presence of double wide (e.g. Japanese) characters."
    // Get the width from representative normal width characters
    _fontWidth = qRound((static_cast<double>(fm.width(QStringLiteral(REPCHAR))) / static_cast<double>(qstrlen(REPCHAR))));

    _fixedFont = true;

    const int fw = fm.width(QLatin1Char(REPCHAR[0]));
    for (unsigned int i = 1; i < qstrlen(REPCHAR); i++) {
        if (fw != fm.width(QLatin1Char(REPCHAR[i]))) {
            _fixedFont = false;
            break;
        }
    }

    if (_fontWidth < 1)
        _fontWidth = 1;

    _fontAscent = fm.ascent();

    emit changedFontMetricSignal(_fontHeight, _fontWidth);
    propagateSize();
    update();
}

void TerminalDisplay::setVTFont(const QFont& f)
{
    QFont newFont(f);

    // In case the provided font doesn't have some specific characters it should
    // fall back to a Monospace fonts.
    newFont.setStyleHint(QFont::TypeWriter);

    QFontMetrics fontMetrics(newFont);

    // This check seems extreme and semi-random
    // TODO: research if these checks are still needed to prevent
    // enormous fonts from being used; consider usage on big TV
    // screens.
    if ((fontMetrics.height() > height()) || (fontMetrics.maxWidth() > width())) {
        // return here will cause the "general" non-fixed width font
        // to be selected
        return;
    }

    // hint that text should be drawn without anti-aliasing.
    // depending on the user's font configuration, this may not be respected
    if (!_antialiasText)
        newFont.setStyleStrategy(QFont::StyleStrategy(newFont.styleStrategy() | QFont::NoAntialias));

    // experimental optimization.  Konsole assumes that the terminal is using a
    // mono-spaced font, in which case kerning information should have an effect.
    // Disabling kerning saves some computation when rendering text.
    newFont.setKerning(false);

    // Konsole cannot handle non-integer font metrics
    newFont.setStyleStrategy(QFont::StyleStrategy(newFont.styleStrategy() | QFont::ForceIntegerMetrics));

    // Try to check that a good font has been loaded.
    // For some fonts, ForceIntegerMetrics causes height() == 0 which
    // will cause Konsole to crash later.
    QFontMetrics fontMetrics2(newFont);
    if ((fontMetrics2.height() < 1)) {
        kDebug()<<"The font "<<newFont.toString()<<" has an invalid height()";
        // Ask for a generic font so at least it is usable.
        // Font listed in profile's dialog will not be updated.
        newFont = QFont(QStringLiteral("Monospace"));
        newFont.setStyleHint(QFont::TypeWriter);
        kDebug()<<"Font changed to "<<newFont.toString();
    }

    QFontInfo fontInfo(newFont);

    if (!fontInfo.fixedPitch()) {
        qWarning() << "Using a variable-width font - this might cause display problems";
    }

    // QFontInfo::fixedPitch() appears to not match QFont::fixedPitch()
    // related?  https://bugreports.qt.io/browse/QTBUG-34082
    if (!fontInfo.exactMatch()) {
        const QChar comma(QLatin1Char(','));
        QString nonMatching = fontInfo.family() % comma %
            QString::number(fontInfo.pointSizeF()) % comma %
            QString::number(fontInfo.pixelSize()) % comma %
            QString::number(static_cast<int>(fontInfo.styleHint())) % comma %
            QString::number(fontInfo.weight()) % comma %
            QString::number(static_cast<int>(fontInfo.style())) % comma %
            QString::number(static_cast<int>(fontInfo.underline())) % comma %
            QString::number(static_cast<int>(fontInfo.strikeOut())) % comma %
            QString::number(static_cast<int>(fontInfo.fixedPitch())) % comma %
            QString::number(static_cast<int>(fontInfo.rawMode()));

        qWarning() << "The font to use in the terminal can not be matched exactly on your system.";
        qWarning()<<" Selected: "<<newFont.toString();
        qWarning()<<" System  : "<<nonMatching;
    }

    QWidget::setFont(newFont);
    fontChange(newFont);
}

void TerminalDisplay::increaseFontSize()
{
    QFont font = getVTFont();
    font.setPointSizeF(font.pointSizeF() + 1);
    setVTFont(font);
}

void TerminalDisplay::decreaseFontSize()
{
    const qreal MinimumFontSize = 6;

    QFont font = getVTFont();
    font.setPointSizeF(std::max(font.pointSizeF() - 1, MinimumFontSize));
    setVTFont(font);
}

void TerminalDisplay::resetFontSize()
{
    const qreal MinimumFontSize = 6;

    QFont font = getVTFont();
    Profile::Ptr currentProfile = SessionManager::instance()->sessionProfile(_sessionController->session());
    const qreal defaultFontSize = currentProfile->font().pointSizeF();

    font.setPointSizeF(std::max(defaultFontSize, MinimumFontSize));
    setVTFont(font);
}

uint TerminalDisplay::lineSpacing() const
{
    return _lineSpacing;
}

void TerminalDisplay::setLineSpacing(uint i)
{
    _lineSpacing = i;
    setVTFont(font()); // Trigger an update.
}


/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                         Accessibility                                     */
/*                                                                           */
/* ------------------------------------------------------------------------- */

namespace Konsole
{
/**
 * This function installs the factory function which lets Qt instantiate the QAccessibleInterface
 * for the TerminalDisplay.
 */
QAccessibleInterface* accessibleInterfaceFactory(const QString &key, QObject *object)
{
    Q_UNUSED(key)
    if (TerminalDisplay *display = qobject_cast<TerminalDisplay*>(object))
        return new TerminalDisplayAccessible(display);
    return nullptr;
}
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                         Constructor / Destructor                          */
/*                                                                           */
/* ------------------------------------------------------------------------- */

TerminalDisplay::TerminalDisplay(QWidget* parent)
    : QWidget(parent)
    , _screenWindow(nullptr)
    , _bellMasked(false)
    , _gridLayout(nullptr)
    , _fontHeight(1)
    , _fontWidth(1)
    , _fontAscent(1)
    , _boldIntense(true)
    , _lines(1)
    , _columns(1)
    , _usedLines(1)
    , _usedColumns(1)
    , _image(nullptr)
    , _resizing(false)
    , _showTerminalSizeHint(true)
    , _bidiEnabled(false)
    , _usesMouseTracking(false)
    , _alternateScrolling(true)
    , _actSel(0)
    , _wordSelectionMode(false)
    , _lineSelectionMode(false)
    , _preserveLineBreaks(true)
    , _columnSelectionMode(false)
    , _autoCopySelectedText(false)
    , _middleClickPasteMode(Enum::PasteFromX11Selection)
    , _scrollbarLocation(Enum::ScrollBarRight)
    , _scrollFullPage(false)
    , _wordCharacters(QStringLiteral(":@-./_~"))
    , _bellMode(Enum::NotifyBell)
    , _allowBlinkingText(true)
    , _allowBlinkingCursor(false)
    , _textBlinking(false)
    , _cursorBlinking(false)
    , _hasTextBlinker(false)
    , _underlineLinks(true)
    , _openLinksByDirectClick(false)
    , _ctrlRequiredForDrag(true)
    , _tripleClickMode(Enum::SelectWholeLine)
    , _possibleTripleClick(false)
    , _resizeWidget(nullptr)
    , _resizeTimer(nullptr)
    , _flowControlWarningEnabled(false)
    , _outputSuspendedLabel(0)
    , _lineSpacing(0)
    , _blendColor(qRgba(0, 0, 0, 0xff))
    , _filterChain(new TerminalImageFilterChain())
    , _filterUpdateRequired(true)
    , _cursorShape(Enum::BlockCursor)
    , _antialiasText(true)
    , _useFontLineCharacters(false)
    , _printerFriendly(false)
    , _sessionController(nullptr)
    , _trimLeadingSpaces(false)
    , _trimTrailingSpaces(false)
    , _margin(1)
    , _centerContents(false)
    , _opacity(1.0)
    , _searchBar(new IncrementalSearchBar(this))
    , _searchResultRect(QRect())
{
    // terminal applications are not designed with Right-To-Left in mind,
    // so the layout is forced to Left-To-Right
    setLayoutDirection(Qt::LeftToRight);

    _contentRect = QRect(_margin, _margin, 1, 1);

    // create scroll bar for scrolling output up and down
    _scrollBar = new QScrollBar(this);
    // set the scroll bar's slider to occupy the whole area of the scroll bar initially
    setScroll(0, 0);
    _scrollBar->setCursor(Qt::ArrowCursor);
    connect(_scrollBar, &QScrollBar::valueChanged,
            this, &Konsole::TerminalDisplay::scrollBarPositionChanged);
    connect(_scrollBar, &QScrollBar::sliderMoved,
            this, &Konsole::TerminalDisplay::viewScrolledByUser);

    // setup timers for blinking text
    _blinkTextTimer = new QTimer(this);
    _blinkTextTimer->setInterval(TEXT_BLINK_DELAY);
    connect(_blinkTextTimer, &QTimer::timeout, this, &Konsole::TerminalDisplay::blinkTextEvent);

    // setup timers for blinking cursor
    _blinkCursorTimer = new QTimer(this);
    _blinkCursorTimer->setInterval(QApplication::cursorFlashTime() / 2);
    connect(_blinkCursorTimer, &QTimer::timeout, this, &Konsole::TerminalDisplay::blinkCursorEvent);

    // hide mouse cursor on keystroke or idle
    KCursor::setAutoHideCursor(this, true);
    setMouseTracking(true);

    setUsesMouseTracking(false);
    setBracketedPasteMode(false);

    setColorTable(ColorScheme::defaultTable);

    // Enable drag and drop support
    setAcceptDrops(true);
    _dragInfo.state = diNone;

    setFocusPolicy(Qt::WheelFocus);

    // enable input method support
    setAttribute(Qt::WA_InputMethodEnabled, true);

    // this is an important optimization, it tells Qt
    // that TerminalDisplay will handle repainting its entire area.
    setAttribute(Qt::WA_OpaquePaintEvent);

    _gridLayout = new QGridLayout;
    _gridLayout->setContentsMargins(0, 0, 0, 0);

    setLayout(_gridLayout);

    new AutoScrollHandler(this);


#ifndef QT_NO_ACCESSIBILITY
    QAccessible::installFactory(Konsole::accessibleInterfaceFactory);
#endif
}

TerminalDisplay::~TerminalDisplay()
{
    disconnect(_blinkTextTimer);
    disconnect(_blinkCursorTimer);

    delete[] _image;

    delete _filterChain;
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                             Display Operations                            */
/*                                                                           */
/* ------------------------------------------------------------------------- */

/**
 A table for emulating the simple (single width) unicode drawing chars.
 It represents the 250x - 257x glyphs. If it's zero, we can't use it.
 if it's not, it's encoded as follows: imagine a 5x5 grid where the points are numbered
 0 to 24 left to top, top to bottom. Each point is represented by the corresponding bit.

 Then, the pixels basically have the following interpretation:
 _|||_
 -...-
 -...-
 -...-
 _|||_

where _ = none
      | = vertical line.
      - = horizontal line.
 */

enum LineEncode {
    TopL  = (1 << 1),
    TopC  = (1 << 2),
    TopR  = (1 << 3),

    LeftT = (1 << 5),
    Int11 = (1 << 6),
    Int12 = (1 << 7),
    Int13 = (1 << 8),
    RightT = (1 << 9),

    LeftC = (1 << 10),
    Int21 = (1 << 11),
    Int22 = (1 << 12),
    Int23 = (1 << 13),
    RightC = (1 << 14),

    LeftB = (1 << 15),
    Int31 = (1 << 16),
    Int32 = (1 << 17),
    Int33 = (1 << 18),
    RightB = (1 << 19),

    BotL  = (1 << 21),
    BotC  = (1 << 22),
    BotR  = (1 << 23)
};

static void drawLineChar(QPainter& paint, int x, int y, int w, int h, uchar code)
{
    //Calculate cell midpoints, end points.
    const int cx = x + w / 2;
    const int cy = y + h / 2. - 0.5;
    const int ex = x + w - 1;
    const int ey = y + h - 1;

    const quint32 toDraw = LineChars[code];

    //Top _lines:
    if (toDraw & TopL)
        paint.drawLine(cx - 1, y, cx - 1, cy - 2);
    if (toDraw & TopC)
        paint.drawLine(cx, y, cx, cy - 2);
    if (toDraw & TopR)
        paint.drawLine(cx + 1, y, cx + 1, cy - 2);

    //Bot _lines:
    if (toDraw & BotL)
        paint.drawLine(cx - 1, cy + 2, cx - 1, ey);
    if (toDraw & BotC)
        paint.drawLine(cx, cy + 2, cx, ey);
    if (toDraw & BotR)
        paint.drawLine(cx + 1, cy + 2, cx + 1, ey);

    //Left _lines:
    if (toDraw & LeftT)
        paint.drawLine(x, cy - 1, cx - 2, cy - 1);
    if (toDraw & LeftC)
        paint.drawLine(x, cy, cx - 2, cy);
    if (toDraw & LeftB)
        paint.drawLine(x, cy + 1, cx - 2, cy + 1);

    //Right _lines:
    if (toDraw & RightT)
        paint.drawLine(cx + 2, cy - 1, ex, cy - 1);
    if (toDraw & RightC)
        paint.drawLine(cx + 2, cy, ex, cy);
    if (toDraw & RightB)
        paint.drawLine(cx + 2, cy + 1, ex, cy + 1);

    //Intersection points.
    if (toDraw & Int11)
        paint.drawPoint(cx - 2, cy - 2);
    if (toDraw & Int12)
        paint.drawPoint(cx - 1, cy - 2);
    if (toDraw & Int13)
        paint.drawPoint(cx - 0, cy - 2);

    if (toDraw & Int21)
        paint.drawPoint(cx - 2, cy - 1);
    if (toDraw & Int22)
        paint.drawPoint(cx - 1, cy - 1);
    if (toDraw & Int23)
        paint.drawPoint(cx - 0, cy - 1);

    if (toDraw & Int31)
        paint.drawPoint(cx - 2, cy);
    if (toDraw & Int32)
        paint.drawPoint(cx - 1, cy);
    if (toDraw & Int33)
        paint.drawPoint(cx - 0, cy);
}

static void drawOtherChar(QPainter& paint, int x, int y, int w, int h, uchar code)
{
    //Calculate cell midpoints, end points.
    const int cx = x + w / 2;
    const int cy = y + h / 2. - 0.5; // Compensate for the translation, to match fonts
    const int ex = x + w - 1;
    const int ey = y + h - 1;

    // Double dashes
    if (0x4C <= code && code <= 0x4F) {
        const int xHalfGap = std::max(w / 15, 1);
        const int yHalfGap = std::max(h / 15, 1);
        switch (code) {
        case 0x4D: // BOX DRAWINGS HEAVY DOUBLE DASH HORIZONTAL
            paint.drawLine(x, cy - 1, cx - xHalfGap - 1, cy - 1);
            paint.drawLine(x, cy + 1, cx - xHalfGap - 1, cy + 1);
            paint.drawLine(cx + xHalfGap, cy - 1, ex, cy - 1);
            paint.drawLine(cx + xHalfGap, cy + 1, ex, cy + 1);
            [[fallthrough]];
        case 0x4C: // BOX DRAWINGS LIGHT DOUBLE DASH HORIZONTAL
            paint.drawLine(x, cy, cx - xHalfGap - 1, cy);
            paint.drawLine(cx + xHalfGap, cy, ex, cy);
            break;
        case 0x4F: // BOX DRAWINGS HEAVY DOUBLE DASH VERTICAL
            paint.drawLine(cx - 1, y, cx - 1, cy - yHalfGap - 1);
            paint.drawLine(cx + 1, y, cx + 1, cy - yHalfGap - 1);
            paint.drawLine(cx - 1, cy + yHalfGap, cx - 1, ey);
            paint.drawLine(cx + 1, cy + yHalfGap, cx + 1, ey);
            [[fallthrough]];
        case 0x4E: // BOX DRAWINGS LIGHT DOUBLE DASH VERTICAL
            paint.drawLine(cx, y, cx, cy - yHalfGap - 1);
            paint.drawLine(cx, cy + yHalfGap, cx, ey);
            break;
        }
    }

    // Rounded corner characters
    else if (0x6D <= code && code <= 0x70) {
        const int r = w * 3 / 8;
        const int d = 2 * r;
        switch (code) {
        case 0x6D: // BOX DRAWINGS LIGHT ARC DOWN AND RIGHT
            paint.drawLine(cx, cy + r, cx, ey);
            paint.drawLine(cx + r, cy, ex, cy);
            paint.drawArc(cx, cy, d, d, 90 * 16, 90 * 16);
            break;
        case 0x6E: // BOX DRAWINGS LIGHT ARC DOWN AND LEFT
            paint.drawLine(cx, cy + r, cx, ey);
            paint.drawLine(x, cy, cx - r, cy);
            paint.drawArc(cx - d, cy, d, d, 0 * 16, 90 * 16);
            break;
        case 0x6F: // BOX DRAWINGS LIGHT ARC UP AND LEFT
            paint.drawLine(cx, y, cx, cy - r);
            paint.drawLine(x, cy, cx - r, cy);
            paint.drawArc(cx - d, cy - d, d, d, 270 * 16, 90 * 16);
            break;
        case 0x70: // BOX DRAWINGS LIGHT ARC UP AND RIGHT
            paint.drawLine(cx, y, cx, cy - r);
            paint.drawLine(cx + r, cy, ex, cy);
            paint.drawArc(cx, cy - d, d, d, 180 * 16, 90 * 16);
            break;
        }
    }

    // Diagonals
    else if (0x71 <= code && code <= 0x73) {
        switch (code) {
        case 0x71: // BOX DRAWINGS LIGHT DIAGONAL UPPER RIGHT TO LOWER LEFT
            paint.drawLine(ex, y, x, ey);
            break;
        case 0x72: // BOX DRAWINGS LIGHT DIAGONAL UPPER LEFT TO LOWER RIGHT
            paint.drawLine(x, y, ex, ey);
            break;
        case 0x73: // BOX DRAWINGS LIGHT DIAGONAL CROSS
            paint.drawLine(ex, y, x, ey);
            paint.drawLine(x, y, ex, ey);
            break;
        }
    }
}

static void drawBlockChar(QPainter& paint, int x, int y, int w, int h, uchar code)
{
    const QColor color = paint.pen().color();

    const float left = x - 0.5;
    const float top = y - 0.5;

    const float cx = left + w / 2;
    const float cy = top + h / 2;
    const float right = x + w - 0.5;
    const float bottom = y + h - 0.5;

    // Default rect fills entire cell
    QRectF rect(left, top, w, h);

    // LOWER ONE EIGHTH BLOCK to LEFT ONE EIGHTH BLOCK
    if (code >= 0x81 && code <= 0x8f) {
        if (code < 0x88) { // Horizontal
            const int height = h * (0x88 - code) / 8;
            rect.setY(top + height);
            rect.setHeight(h - height);
        } else if (code > 0x88) { // Vertical
            const int width = w * (0x90 - code) / 8;
            rect.setWidth(width);
        }

        paint.fillRect(rect, color);

        return;
    }

    // Combinations of quarter squares
    // LEFT ONE EIGHTH BLOCK to QUADRANT UPPER RIGHT AND LOWER LEFT AND LOWER RIGHT
    if (code >= 0x96 && code <= 0x9F) {
        bool upperLeft = false, upperRight = false,
             lowerLeft = false, lowerRight = false;

        switch(code) {
        case 0x96:
            lowerLeft = true;
            break;
        case 0x97:
            lowerRight = true;
            break;
        case 0x98:
            upperLeft = true;
            break;
        case 0x99:
            upperLeft = true;
            lowerLeft = true;
            lowerRight = true;
            break;
        case 0x9a:
            upperLeft = true;
            lowerRight = true;
            break;
        case 0x9b:
            upperLeft = true;
            upperRight = true;
            lowerLeft = true;
            break;
        case 0x9c:
            upperLeft = true;
            upperRight = true;
            lowerRight = true;
            break;
        case 0x9d:
            upperRight = true;
            break;
        case 0x9e:
            upperRight = true;
            lowerLeft = true;
            break;
        case 0x9f:
            upperRight = true;
            lowerLeft = true;
            lowerRight = true;
            break;
        default:
            break;
        }

        if (upperLeft) {
            paint.fillRect(QRectF(QPointF(left, top), QPointF(cx, cy)), color);
        }
        if (upperRight) {
            paint.fillRect(QRectF(QPointF(cx, top), QPointF(right, cy)), color);
        }
        if (lowerLeft) {
            paint.fillRect(QRectF(QPointF(left, cy), QPointF(cx, bottom)), color);
        }
        if (lowerRight) {
            paint.fillRect(QRectF(QPointF(cx, cy), QPointF(right, bottom)), color);
        }

        return;
    }

    // And the random stuff
    switch(code) {
    case 0x80: // Top half block
        rect.setHeight(h / 2);
        paint.fillRect(rect, color);
        return;
    case 0x90: // Right half block
        paint.fillRect(QRectF(QPointF(cx, top), QPointF(right, bottom)), color);
        return;
    case 0x94: // Top one eighth block
        rect.setHeight(h / 8);
        paint.fillRect(rect, color);
        return;
    case 0x95: { // Right one eighth block
        const float width = 7 * w / 8;
        rect.setX(left + width);
        rect.setWidth(w - width);
        paint.fillRect(rect, color);
        return;
    }
    case 0x91: // Light shade
        paint.fillRect(rect, QBrush(color, Qt::Dense6Pattern));
        return;
    case 0x92: // Medium shade
        paint.fillRect(rect, QBrush(color, Qt::Dense4Pattern));
        return;
    case 0x93: // Dark shade
        paint.fillRect(rect, QBrush(color, Qt::Dense2Pattern));
        return;

    default:
        break;
    }
}

void TerminalDisplay::drawLineCharString(QPainter& painter, int x, int y, const QString& str,
        const Character* attributes)
{
    painter.save();

    // For antialiasing, we need to shift it so the single pixel width is in the middle
    painter.translate(0.5, 0.5);

    if ((attributes->rendition & RE_BOLD) && _boldIntense) {
        QPen boldPen(painter.pen());
        boldPen.setWidth(4);
        painter.setPen(boldPen);
    }

    for (int i = 0 ; i < str.length(); i++) {
        const uchar code = str[i].cell();

        if (code >= 0x80 && code <= 0x9F) { // UPPER HALF BLOCK to QUADRANT UPPER RIGHT AND LOWER LEFT AND LOWER RIGHT
            drawBlockChar(painter, x + (_fontWidth * i), y, _fontWidth, _fontHeight, code);
        } else if (LineChars[code] != 0u) {
            drawLineChar(painter, x + (_fontWidth * i), y, _fontWidth, _fontHeight, code);
        } else {
            drawOtherChar(painter, x + (_fontWidth * i), y, _fontWidth, _fontHeight, code);
        }
    }

    painter.restore();
}

void TerminalDisplay::setKeyboardCursorShape(Enum::CursorShapeEnum shape)
{
    _cursorShape = shape;
}
Enum::CursorShapeEnum TerminalDisplay::keyboardCursorShape() const
{
    return _cursorShape;
}

void TerminalDisplay::setCursorStyle(Enum::CursorShapeEnum shape, bool isBlinking)
{
    setKeyboardCursorShape(shape);

    setBlinkingCursorEnabled(isBlinking);

    // when the cursor shape and blinking state are changed via the
    // Set Cursor Style (DECSCUSR) escape sequences in vim, and if the
    // cursor isn't set to blink, the cursor shape doesn't actually
    // change until the cursor is moved by the user; calling update()
    // makes the cursor shape get updated sooner.
    if (!isBlinking) {
        update();
    }
}
void TerminalDisplay::resetCursorStyle()
{
    Q_ASSERT(_sessionController != nullptr);
    Q_ASSERT(!_sessionController->session().isNull());

    Profile::Ptr currentProfile = SessionManager::instance()->sessionProfile(_sessionController->session());

    if (currentProfile != nullptr) {
        Enum::CursorShapeEnum shape = static_cast<Enum::CursorShapeEnum>(currentProfile->property<int>(Profile::CursorShape));

        setKeyboardCursorShape(shape);
        setBlinkingCursorEnabled(currentProfile->blinkingCursorEnabled());
    }
}

void TerminalDisplay::setKeyboardCursorColor(const QColor& color)
{
    _cursorColor = color;
}
QColor TerminalDisplay::keyboardCursorColor() const
{
    return _cursorColor;
}

void TerminalDisplay::setOpacity(qreal opacity)
{
    QColor color(_blendColor);
    color.setAlphaF(opacity);
    _opacity = opacity;

    // enable automatic background filling to prevent the display
    // flickering if there is no transparency
    /*if ( color.alpha() == 255 )
    {
        setAutoFillBackground(true);
    }
    else
    {
        setAutoFillBackground(false);
    }*/

    _blendColor = color.rgba();
}

void TerminalDisplay::setWallpaper(ColorSchemeWallpaper::Ptr p)
{
    _wallpaper = p;
}

void TerminalDisplay::drawBackground(QPainter& painter, const QRect& rect, const QColor& backgroundColor, bool useOpacitySetting)
{
    // the area of the widget showing the contents of the terminal display is drawn
    // using the background color from the color scheme set with setColorTable()
    //
    // the area of the widget behind the scroll-bar is drawn using the background
    // brush from the scroll-bar's palette, to give the effect of the scroll-bar
    // being outside of the terminal display and visual consistency with other KDE
    // applications.

    if (useOpacitySetting && !_wallpaper->isNull() &&
            _wallpaper->draw(painter, rect, _opacity)) {
    } else if (qAlpha(_blendColor) < 0xff && useOpacitySetting) {
#if defined(Q_OS_MAC)
        // TODO - On MacOS, using CompositionMode doesn't work.  Altering the
        //        transparency in the color scheme alters the brightness.
        painter.fillRect(rect, backgroundColor);
#else
        QColor color(backgroundColor);
        color.setAlpha(qAlpha(_blendColor));

        const QPainter::CompositionMode originalMode = painter.compositionMode();
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect, color);
        painter.setCompositionMode(originalMode);
#endif
    } else {
        painter.fillRect(rect, backgroundColor);
    }
}

void TerminalDisplay::drawCursor(QPainter& painter,
                                 const QRect& rect,
                                 const QColor& foregroundColor,
                                 const QColor& /*backgroundColor*/,
                                 bool& invertCharacterColor)
{
    // don't draw cursor which is currently blinking
    if (_cursorBlinking)
        return;

    // shift rectangle top down one pixel to leave some space
    // between top and bottom
    QRectF cursorRect = rect.adjusted(0, 1, 0, 0);

    QColor cursorColor = _cursorColor.isValid() ? _cursorColor : foregroundColor;
    painter.setPen(cursorColor);

    if (_cursorShape == Enum::BlockCursor) {
        // draw the cursor outline, adjusting the area so that
        // it is draw entirely inside 'rect'
        int penWidth = std::max(1, painter.pen().width());
        painter.drawRect(cursorRect.adjusted(penWidth / 2 + 0.5,
                                             penWidth / 2 + 0.5,
                                             - penWidth / 2 - penWidth % 2 + 0.5,
                                             - penWidth / 2 - penWidth % 2 + 0.5));

        // draw the cursor body only when the widget has focus
        if (hasFocus()) {
            painter.fillRect(cursorRect, cursorColor);

            if (!_cursorColor.isValid()) {
                // invert the color used to draw the text to ensure that the character at
                // the cursor position is readable
                invertCharacterColor = true;
            }
        }
    } else if (_cursorShape == Enum::UnderlineCursor) {
        painter.drawLine(cursorRect.left(),
                         cursorRect.bottom(),
                         cursorRect.right(),
                         cursorRect.bottom());

    } else if (_cursorShape == Enum::IBeamCursor) {
        painter.drawLine(cursorRect.left(),
                         cursorRect.top(),
                         cursorRect.left(),
                         cursorRect.bottom());
    }
}

void TerminalDisplay::drawCharacters(QPainter& painter,
                                     const QRect& rect,
                                     const QString& text,
                                     const Character* style,
                                     bool invertCharacterColor)
{
    // don't draw text which is currently blinking
    if (_textBlinking && (style->rendition & RE_BLINK))
        return;

    // don't draw concealed characters
    if (style->rendition & RE_CONCEAL)
        return;

    // setup bold and underline
    bool useBold = ((style->rendition & RE_BOLD) && _boldIntense) || font().bold();
    const bool useUnderline = style->rendition & RE_UNDERLINE || font().underline();
    const bool useItalic = style->rendition & RE_ITALIC || font().italic();
    const bool useStrikeOut = style->rendition & RE_STRIKEOUT || font().strikeOut();
    const bool useOverline = style->rendition & RE_OVERLINE || font().overline();

    QFont font = painter.font();
    if (font.bold() != useBold
            || font.underline() != useUnderline
            || font.italic() != useItalic
            || font.strikeOut() != useStrikeOut
            || font.overline() != useOverline) {
        font.setBold(useBold);
        font.setUnderline(useUnderline);
        font.setItalic(useItalic);
        font.setStrikeOut(useStrikeOut);
        font.setOverline(useOverline);
        painter.setFont(font);
    }

    // setup pen
    const CharacterColor& textColor = (invertCharacterColor ? style->backgroundColor : style->foregroundColor);
    const QColor color = textColor.color(_colorTable);
    QPen pen = painter.pen();
    if (pen.color() != color) {
        pen.setColor(color);
        painter.setPen(pen);
    }

    // draw text
    if (isLineCharString(text) && !_useFontLineCharacters) {
        drawLineCharString(painter, rect.x(), rect.y(), text, style);
    } else {
        // Force using LTR as the document layout for the terminal area, because
        // there is no use cases for RTL emulator and RTL terminal application.
        //
        // This still allows RTL characters to be rendered in the RTL way.
        painter.setLayoutDirection(Qt::LeftToRight);

        painter.setClipRect(rect);
        if (_bidiEnabled) {
            painter.drawText(rect.x(), rect.y() + _fontAscent + _lineSpacing, text);
        } else {
            painter.drawText(rect.x(), rect.y() + _fontAscent + _lineSpacing, LTR_OVERRIDE_CHAR + text);
        }
        painter.setClipping(false);
    }
}

void TerminalDisplay::drawTextFragment(QPainter& painter ,
                                       const QRect& rect,
                                       const QString& text,
                                       const Character* style)
{
    // setup painter
    const QColor foregroundColor = style->foregroundColor.color(_colorTable);
    const QColor backgroundColor = style->backgroundColor.color(_colorTable);

    // draw background if different from the display's background color
    if (backgroundColor != palette().background().color())
        drawBackground(painter, rect, backgroundColor,
                       false /* do not use transparency */);

    // draw cursor shape if the current character is the cursor
    // this may alter the foreground and background colors
    bool invertCharacterColor = false;
    if (style->rendition & RE_CURSOR)
        drawCursor(painter, rect, foregroundColor, backgroundColor, invertCharacterColor);

    // draw text
    drawCharacters(painter, rect, text, style, invertCharacterColor);
}

void TerminalDisplay::drawPrinterFriendlyTextFragment(QPainter& painter,
        const QRect& rect,
        const QString& text,
        const Character* style)
{
    // Set the colors used to draw to black foreground and white
    // background for printer friendly output when printing
    Character print_style = *style;
    print_style.foregroundColor = CharacterColor(COLOR_SPACE_RGB, 0x00000000);
    print_style.backgroundColor = CharacterColor(COLOR_SPACE_RGB, 0xFFFFFFFF);

    // draw text
    drawCharacters(painter, rect, text, &print_style, false);
}

// scrolls the image by 'lines', down if lines > 0 or up otherwise.
//
// the terminal emulation keeps track of the scrolling of the character
// image as it receives input, and when the view is updated, it calls scrollImage()
// with the final scroll amount.  this improves performance because scrolling the
// display is much cheaper than re-rendering all the text for the
// part of the image which has moved up or down.
// Instead only new lines have to be drawn
void TerminalDisplay::scrollImage(int lines , const QRect& screenWindowRegion)
{
    // return if there is nothing to do
    if ((lines == 0) || (_image == nullptr)) {
        return;
    }

    // if the flow control warning is enabled this will interfere with the
    // scrolling optimizations and cause artifacts.  the simple solution here
    // is to just disable the optimization whilst it is visible
    if (_outputSuspendedLabel && _outputSuspendedLabel->isVisible())
        return;

    // constrain the region to the display
    // the bottom of the region is capped to the number of lines in the display's
    // internal image - 2, so that the height of 'region' is strictly less
    // than the height of the internal image.
    QRect region = screenWindowRegion;
    region.setBottom(std::min(region.bottom(), _lines - 2));

    // return if there is nothing to do
    if (!region.isValid()
            || (region.top() + abs(lines)) >= region.bottom()
            || _lines <= region.height()) return;

    // hide terminal size label to prevent it being scrolled
    if (_resizeWidget && _resizeWidget->isVisible())
        _resizeWidget->hide();

    // Note:  With Qt 4.4 the left edge of the scrolled area must be at 0
    // to get the correct (newly exposed) part of the widget repainted.
    //
    // The right edge must be before the left edge of the scroll bar to
    // avoid triggering a repaint of the entire widget, the distance is
    // given by SCROLLBAR_CONTENT_GAP
    //
    // Set the QT_FLUSH_PAINT environment variable to '1' before starting the
    // application to monitor repainting.
    //
    const int scrollBarWidth = _scrollBar->isHidden() ? 0 : _scrollBar->width();
    const int SCROLLBAR_CONTENT_GAP = 1;
    QRect scrollRect;
    if (_scrollbarLocation == Enum::ScrollBarLeft) {
        scrollRect.setLeft(scrollBarWidth + SCROLLBAR_CONTENT_GAP);
        scrollRect.setRight(width());
    } else {
        scrollRect.setLeft(0);
        scrollRect.setRight(width() - scrollBarWidth - SCROLLBAR_CONTENT_GAP);
    }
    void* firstCharPos = &_image[ region.top() * _columns ];
    void* lastCharPos = &_image[(region.top() + abs(lines)) * _columns ];

    const int top = _contentRect.top() + (region.top() * _fontHeight);
    const int linesToMove = region.height() - abs(lines);
    const int bytesToMove = linesToMove * _columns * sizeof(Character);

    Q_ASSERT(linesToMove > 0);
    Q_ASSERT(bytesToMove > 0);

    //scroll internal image
    if (lines > 0) {
        // check that the memory areas that we are going to move are valid
        Q_ASSERT((char*)lastCharPos + bytesToMove <
                 (char*)(_image + (_lines * _columns)));

        Q_ASSERT((lines * _columns) < _imageSize);

        //scroll internal image down
        std::memmove(firstCharPos , lastCharPos , bytesToMove);

        //set region of display to scroll
        scrollRect.setTop(top);
    } else {
        // check that the memory areas that we are going to move are valid
        Q_ASSERT((char*)firstCharPos + bytesToMove <
                 (char*)(_image + (_lines * _columns)));

        //scroll internal image up
        std::memmove(lastCharPos , firstCharPos , bytesToMove);

        //set region of the display to scroll
        scrollRect.setTop(top + abs(lines) * _fontHeight);
    }
    scrollRect.setHeight(linesToMove * _fontHeight);

    Q_ASSERT(scrollRect.isValid() && !scrollRect.isEmpty());

    //scroll the display vertically to match internal _image
    scroll(0 , _fontHeight * (-lines) , scrollRect);
}

QRegion TerminalDisplay::hotSpotRegion() const
{
    QRegion region;
    foreach(Filter::HotSpot * hotSpot , _filterChain->hotSpots()) {
        QRect r;
        if (hotSpot->startLine() == hotSpot->endLine()) {
            r.setLeft(hotSpot->startColumn());
            r.setTop(hotSpot->startLine());
            r.setRight(hotSpot->endColumn());
            r.setBottom(hotSpot->endLine());
            region |= imageToWidget(r);
        } else {
            r.setLeft(hotSpot->startColumn());
            r.setTop(hotSpot->startLine());
            r.setRight(_columns);
            r.setBottom(hotSpot->startLine());
            region |= imageToWidget(r);
            for (int line = hotSpot->startLine() + 1 ; line < hotSpot->endLine() ; line++) {
                r.setLeft(0);
                r.setTop(line);
                r.setRight(_columns);
                r.setBottom(line);
                region |= imageToWidget(r);
            }
            r.setLeft(0);
            r.setTop(hotSpot->endLine());
            r.setRight(hotSpot->endColumn());
            r.setBottom(hotSpot->endLine());
            region |= imageToWidget(r);
        }
    }
    return region;
}

void TerminalDisplay::processFilters()
{
    if (_screenWindow.isNull()) {
        return;
    }

    if (!_filterUpdateRequired) {
        return;
    }

    QRegion preUpdateHotSpots = hotSpotRegion();

    // use _screenWindow->getImage() here rather than _image because
    // other classes may call processFilters() when this display's
    // ScreenWindow emits a scrolled() signal - which will happen before
    // updateImage() is called on the display and therefore _image is
    // out of date at this point
    _filterChain->setImage(_screenWindow->getImage(),
                           _screenWindow->windowLines(),
                           _screenWindow->windowColumns(),
                           _screenWindow->getLineProperties());
    _filterChain->process();

    QRegion postUpdateHotSpots = hotSpotRegion();

    update(preUpdateHotSpots | postUpdateHotSpots);
    _filterUpdateRequired = false;
}

void TerminalDisplay::updateImage()
{
    if (_screenWindow.isNull())
        return;

    // optimization - scroll the existing image where possible and
    // avoid expensive text drawing for parts of the image that
    // can simply be moved up or down
    if (_wallpaper->isNull() && !_searchBar->isVisible()) {
        scrollImage(_screenWindow->scrollCount() ,
                    _screenWindow->scrollRegion());
    }

    if (!_image) {
        // Create _image.
        // The emitted changedContentSizeSignal also leads to getImage being recreated, so do this first.
        updateImageSize();
    }

    Character* const newimg = _screenWindow->getImage();
    const int lines = _screenWindow->windowLines();
    const int columns = _screenWindow->windowColumns();

    setScroll(_screenWindow->currentLine() , _screenWindow->lineCount());

    Q_ASSERT(_usedLines <= _lines);
    Q_ASSERT(_usedColumns <= _columns);

    int y, x, len;

    const QPoint tL  = contentsRect().topLeft();
    const int    tLx = tL.x();
    const int    tLy = tL.y();
    _hasTextBlinker = false;

    CharacterColor cf;       // undefined

    const int linesToUpdate = std::min(_lines, std::max(0, lines));
    const int columnsToUpdate = std::min(_columns, std::max(0, columns));

    char* dirtyMask = new char[columnsToUpdate + 2];
    QRegion dirtyRegion;

    // debugging variable, this records the number of lines that are found to
    // be 'dirty' ( ie. have changed from the old _image to the new _image ) and
    // which therefore need to be repainted
    int dirtyLineCount = 0;

    for (y = 0; y < linesToUpdate; ++y) {
        const Character* currentLine = &_image[y * _columns];
        const Character* const newLine = &newimg[y * columns];

        bool updateLine = false;

        // The dirty mask indicates which characters need repainting. We also
        // mark surrounding neighbors dirty, in case the character exceeds
        // its cell boundaries
        std::memset(dirtyMask, 0, columnsToUpdate + 2);

        for (x = 0 ; x < columnsToUpdate ; ++x) {
            if (newLine[x] != currentLine[x]) {
                dirtyMask[x] = true;
            }
        }

        if (!_resizing) // not while _resizing, we're expecting a paintEvent
            for (x = 0; x < columnsToUpdate; ++x) {
                _hasTextBlinker |= (newLine[x].rendition & RE_BLINK);

                // Start drawing if this character or the next one differs.
                // We also take the next one into account to handle the situation
                // where characters exceed their cell width.
                if (dirtyMask[x]) {
                    if (!newLine[x + 0].character)
                        continue;
                    const bool lineDraw = newLine[x + 0].isLineChar();
                    const bool doubleWidth = (x + 1 == columnsToUpdate) ? false : (newLine[x + 1].character == 0);
                    const RenditionFlags cr = newLine[x].rendition;
                    const CharacterColor clipboard = newLine[x].backgroundColor;
                    if (newLine[x].foregroundColor != cf) cf = newLine[x].foregroundColor;
                    const int lln = columnsToUpdate - x;
                    for (len = 1; len < lln; ++len) {
                        const Character& ch = newLine[x + len];

                        if (!ch.character)
                            continue; // Skip trailing part of multi-col chars.

                        const bool nextIsDoubleWidth = (x + len + 1 == columnsToUpdate) ? false : (newLine[x + len + 1].character == 0);

                        if (ch.foregroundColor != cf ||
                                ch.backgroundColor != clipboard ||
                                (ch.rendition & ~RE_EXTENDED_CHAR) != (cr & ~RE_EXTENDED_CHAR) ||
                                !dirtyMask[x + len] ||
                                ch.isLineChar() != lineDraw ||
                                nextIsDoubleWidth != doubleWidth)
                            break;
                    }

                    const bool saveFixedFont = _fixedFont;
                    if (lineDraw)
                        _fixedFont = false;
                    if (doubleWidth)
                        _fixedFont = false;

                    updateLine = true;

                    _fixedFont = saveFixedFont;
                    x += len - 1;
                }
            }

        //both the top and bottom halves of double height _lines must always be redrawn
        //although both top and bottom halves contain the same characters, only
        //the top one is actually
        //drawn.
        if (_lineProperties.count() > y)
            updateLine |= (_lineProperties[y] & LINE_DOUBLEHEIGHT);

        // if the characters on the line are different in the old and the new _image
        // then this line must be repainted.
        if (updateLine) {
            dirtyLineCount++;

            // add the area occupied by this line to the region which needs to be
            // repainted
            QRect dirtyRect = QRect(_contentRect.left() + tLx ,
                                    _contentRect.top() + tLy + _fontHeight * y ,
                                    _fontWidth * columnsToUpdate ,
                                    _fontHeight);

            dirtyRegion |= dirtyRect;
        }

        // replace the line of characters in the old _image with the
        // current line of the new _image
        std::memcpy((void*)currentLine, (const void*)newLine, columnsToUpdate * sizeof(Character));
    }

    // if the new _image is smaller than the previous _image, then ensure that the area
    // outside the new _image is cleared
    if (linesToUpdate < _usedLines) {
        dirtyRegion |= QRect(_contentRect.left() + tLx ,
                             _contentRect.top() + tLy + _fontHeight * linesToUpdate ,
                             _fontWidth * _columns ,
                             _fontHeight * (_usedLines - linesToUpdate));
    }
    _usedLines = linesToUpdate;

    if (columnsToUpdate < _usedColumns) {
        dirtyRegion |= QRect(_contentRect.left() + tLx + columnsToUpdate * _fontWidth ,
                             _contentRect.top() + tLy ,
                             _fontWidth * (_usedColumns - columnsToUpdate) ,
                             _fontHeight * _lines);
    }
    _usedColumns = columnsToUpdate;

    dirtyRegion |= _inputMethodData.previousPreeditRect;

    if ((_screenWindow->currentResultLine() != -1) && (_screenWindow->scrollCount() != 0)) {
        // De-highlight previous result region
        dirtyRegion |= _searchResultRect;
        // Highlight new result region
        dirtyRegion |= QRect(0, _contentRect.top() + (_screenWindow->currentResultLine() - _screenWindow->currentLine()) * _fontHeight,
                             _columns * _fontWidth, _fontHeight);
    }
    _screenWindow->resetScrollCount();

    // update the parts of the display which have changed
    update(dirtyRegion);

    if (_allowBlinkingText && _hasTextBlinker && !_blinkTextTimer->isActive()) {
        _blinkTextTimer->start();
    }
    if (!_hasTextBlinker && _blinkTextTimer->isActive()) {
        _blinkTextTimer->stop();
        _textBlinking = false;
    }
    delete[] dirtyMask;

#if QT_VERSION >= 0x040800 // added in Qt 4.8.0
#ifndef QT_NO_ACCESSIBILITY
    QAccessible::updateAccessibility(this, 0, QAccessible::TextUpdated);
    QAccessible::updateAccessibility(this, 0, QAccessible::TextCaretMoved);
#endif
#endif
}

void TerminalDisplay::showResizeNotification()
{
    if (_showTerminalSizeHint && isVisible()) {
        if (!_resizeWidget) {
            _resizeWidget = new QLabel(i18n("Size: XXX x XXX"), this);
            _resizeWidget->setMinimumWidth(_resizeWidget->fontMetrics().width(i18n("Size: XXX x XXX")));
            _resizeWidget->setMinimumHeight(_resizeWidget->sizeHint().height());
            _resizeWidget->setAlignment(Qt::AlignCenter);

            _resizeWidget->setStyleSheet(QStringLiteral("background-color:palette(window);border-style:solid;border-width:1px;border-color:palette(dark)"));

            _resizeTimer = new QTimer(this);
            _resizeTimer->setInterval(SIZE_HINT_DURATION);
            _resizeTimer->setSingleShot(true);
            connect(_resizeTimer, &QTimer::timeout, _resizeWidget, &QLabel::hide);
        }
        QString sizeStr = i18n("Size: %1 x %2", _columns, _lines);
        _resizeWidget->setText(sizeStr);
        _resizeWidget->move((width() - _resizeWidget->width()) / 2,
                            (height() - _resizeWidget->height()) / 2 + 20);
        _resizeWidget->show();
        _resizeTimer->start();
    }
}

void TerminalDisplay::paintEvent(QPaintEvent* pe)
{
    QPainter paint(this);

    // Determine which characters should be repainted (1 region unit = 1 character)
    QRegion dirtyImageRegion;
    for(const QRect & rect: (pe->region() & contentsRect()).rects()) {
        dirtyImageRegion += widgetToImage(rect);
        drawBackground(paint, rect, palette().background().color(), true /* use opacity setting */);
    }

    paint.setRenderHint(QPainter::Antialiasing, _antialiasText);

    for(const QRect & rect: dirtyImageRegion.rects()) {
        drawContents(paint, rect);
    }
    drawCurrentResultRect(paint);
    drawInputMethodPreeditString(paint, preeditRect());
    paintFilters(paint);
}

void TerminalDisplay::printContent(QPainter& painter, bool friendly)
{
    // Reinitialize the font with the printers paint device so the font
    // measurement calculations will be done correctly
    QFont savedFont = getVTFont();
    QFont font(savedFont, painter.device());
    painter.setFont(font);
    setVTFont(font);

    QRect rect(0, 0, _usedColumns, _usedLines);

    _printerFriendly = friendly;
    if (!friendly) {
        drawBackground(painter, rect, getBackgroundColor(),
                       true /* use opacity setting */);
    }
    drawContents(painter, rect);
    _printerFriendly = false;
    setVTFont(savedFont);
}

QPoint TerminalDisplay::cursorPosition() const
{
    if (!_screenWindow.isNull())
        return _screenWindow->cursorPosition();
    else
        return QPoint(0, 0);
}

inline bool TerminalDisplay::isCursorOnDisplay() const
{
    return cursorPosition().x() < _columns &&
           cursorPosition().y() < _lines;
}

FilterChain* TerminalDisplay::filterChain() const
{
    return _filterChain;
}

void TerminalDisplay::paintFilters(QPainter& painter)
{
    if (_filterUpdateRequired) {
        return;
    }

    // get color of character under mouse and use it to draw
    // lines for filters
    QPoint cursorPos = mapFromGlobal(QCursor::pos());
    int cursorLine;
    int cursorColumn;

    getCharacterPosition(cursorPos, cursorLine, cursorColumn, false);
    Character cursorCharacter = _image[loc(std::min(cursorColumn, _columns - 1), cursorLine)];

    painter.setPen(QPen(cursorCharacter.foregroundColor.color(colorTable())));

    // iterate over hotspots identified by the display's currently active filters
    // and draw appropriate visuals to indicate the presence of the hotspot

    QList<Filter::HotSpot*> spots = _filterChain->hotSpots();
    for(Filter::HotSpot* spot: spots) {
        QRegion region;
        if (_underlineLinks && spot->type() == Filter::HotSpot::Link) {
            QRect r;
            if (spot->startLine() == spot->endLine()) {
                r.setCoords(spot->startColumn()*_fontWidth + _contentRect.left(),
                            spot->startLine()*_fontHeight + _contentRect.top(),
                            (spot->endColumn())*_fontWidth + _contentRect.left() - 1,
                            (spot->endLine() + 1)*_fontHeight + _contentRect.top() - 1);
                region |= r;
            } else {
                r.setCoords(spot->startColumn()*_fontWidth + _contentRect.left(),
                            spot->startLine()*_fontHeight + _contentRect.top(),
                            (_columns)*_fontWidth + _contentRect.left() - 1,
                            (spot->startLine() + 1)*_fontHeight + _contentRect.top() - 1);
                region |= r;
                for (int line = spot->startLine() + 1 ; line < spot->endLine() ; line++) {
                    r.setCoords(0 * _fontWidth + _contentRect.left(),
                                line * _fontHeight + _contentRect.top(),
                                (_columns)*_fontWidth + _contentRect.left() - 1,
                                (line + 1)*_fontHeight + _contentRect.top() - 1);
                    region |= r;
                }
                r.setCoords(0 * _fontWidth + _contentRect.left(),
                            spot->endLine()*_fontHeight + _contentRect.top(),
                            (spot->endColumn())*_fontWidth + _contentRect.left() - 1,
                            (spot->endLine() + 1)*_fontHeight + _contentRect.top() - 1);
                region |= r;
            }
        }

        for (int line = spot->startLine() ; line <= spot->endLine() ; line++) {
            int startColumn = 0;
            int endColumn = _columns - 1; // TODO use number of _columns which are actually
            // occupied on this line rather than the width of the
            // display in _columns

            // Check image size so _image[] is valid (see makeImage)
            if (endColumn >= _columns || line >= _lines)
                break;

            // ignore whitespace at the end of the lines
            while (_image[loc(endColumn, line)].isSpace() && endColumn > 0)
                endColumn--;

            // increment here because the column which we want to set 'endColumn' to
            // is the first whitespace character at the end of the line
            endColumn++;

            if (line == spot->startLine())
                startColumn = spot->startColumn();
            if (line == spot->endLine())
                endColumn = spot->endColumn();

            // TODO: resolve this comment with the new margin/center code
            // subtract one pixel from
            // the right and bottom so that
            // we do not overdraw adjacent
            // hotspots
            //
            // subtracting one pixel from all sides also prevents an edge case where
            // moving the mouse outside a link could still leave it underlined
            // because the check below for the position of the cursor
            // finds it on the border of the target area
            QRect r;
            r.setCoords(startColumn * _fontWidth + _contentRect.left(),
                        line * _fontHeight + _contentRect.top(),
                        endColumn * _fontWidth + _contentRect.left() - 1,
                        (line + 1)*_fontHeight + _contentRect.top() - 1);
            // Underline link hotspots
            if (_underlineLinks && spot->type() == Filter::HotSpot::Link) {
                QFontMetrics metrics(font());

                // find the baseline (which is the invisible line that the characters in the font sit on,
                // with some having tails dangling below)
                const int baseline = r.bottom() - metrics.descent();
                // find the position of the underline below that
                const int underlinePos = baseline + metrics.underlinePos();
                if (region.contains(mapFromGlobal(QCursor::pos()))) {
                    painter.drawLine(r.left() , underlinePos ,
                                     r.right() , underlinePos);
                }
                // Marker hotspots simply have a transparent rectangular shape
                // drawn on top of them
            } else if (spot->type() == Filter::HotSpot::Marker) {
                //TODO - Do not use a hardcoded color for this
                const bool isCurrentResultLine = (_screenWindow->currentResultLine() == (spot->startLine() + _screenWindow->currentLine()));
                QColor color = isCurrentResultLine ? QColor(255, 255, 0, 120) : QColor(255, 0, 0, 120);
                painter.fillRect(r, color);
            }
        }
    }
}

inline static bool isRtl(const Character &chr) {
    uint c = 0;
    if ((chr.rendition & RE_EXTENDED_CHAR) == 0) {
        c = chr.character;
    } else {
        ushort extendedCharLength = 0;
        const uint* chars = ExtendedCharTable::instance.lookupExtendedChar(chr.character, extendedCharLength);
        if (chars != nullptr) {
            c = chars[0];
        }
    }

    switch(QChar::direction(c)) {
    case QChar::DirR:
    case QChar::DirAL:
    case QChar::DirRLE:
    //case QChar::DirRLI:
    case QChar::DirRLO:
        return true;
    default:
        return false;
    }
}

void TerminalDisplay::drawContents(QPainter& paint, const QRect& rect)
{
    const int numberOfColumns = _usedColumns;
    QVector<uint> univec;
    univec.reserve(numberOfColumns);
    for (int y = rect.y(); y <= rect.bottom(); y++) {
        int x = rect.x();
        if ((_image[loc(rect.x(), y)].character == 0u) && (x != 0)) {
            x--; // Search for start of multi-column character
        }
        for (; x <= rect.right(); x++) {
            int len = 1;
            int p = 0;

            // reset our buffer to the number of columns
            int bufferSize = numberOfColumns;
            univec.resize(bufferSize);
            uint *disstrU = univec.data();

            // is this a single character or a sequence of characters ?
            if (_image[loc(x, y)].rendition & RE_EXTENDED_CHAR) {
                // sequence of characters
                ushort extendedCharLength = 0;
                const uint* chars = ExtendedCharTable::instance.lookupExtendedChar(_image[loc(x, y)].character, extendedCharLength);
                if (chars) {
                    Q_ASSERT(extendedCharLength > 1);
                    bufferSize += extendedCharLength - 1;
                    univec.resize(bufferSize);
                    disstrU = univec.data();
                    for (int index = 0 ; index < extendedCharLength ; index++) {
                        Q_ASSERT(p < bufferSize);
                        disstrU[p++] = chars[index];
                    }
                }
            } else {
                // single character
                const uint c = _image[loc(x, y)].character;
                if (c) {
                    Q_ASSERT(p < bufferSize);
                    disstrU[p++] = c;
                }
            }

            const bool lineDraw = _image[loc(x, y)].isLineChar();
            const bool doubleWidth = (_image[std::min(loc(x, y) + 1, _imageSize - 1)].character == 0);
            const CharacterColor currentForeground = _image[loc(x, y)].foregroundColor;
            const CharacterColor currentBackground = _image[loc(x, y)].backgroundColor;
            const RenditionFlags currentRendition = _image[loc(x, y)].rendition;
            const bool rtl = isRtl(_image[loc(x, y)]);

            if(_image[loc(x, y)].character <= 0x7e || rtl) {
                while (x + len <= rect.right() &&
                        _image[loc(x + len, y)].foregroundColor == currentForeground &&
                        _image[loc(x + len, y)].backgroundColor == currentBackground &&
                        (_image[loc(x + len, y)].rendition & ~RE_EXTENDED_CHAR) == (currentRendition & ~RE_EXTENDED_CHAR) &&
                        (_image[ std::min(loc(x + len, y) + 1, _imageSize - 1) ].character == 0) == doubleWidth &&
                        _image[loc(x + len, y)].isLineChar() == lineDraw &&
                        (_image[loc(x + len, y)].character <= 0x7e || rtl)) {
                    const uint c = _image[loc(x + len, y)].character;
                    if ((_image[loc(x + len, y)].rendition & RE_EXTENDED_CHAR) != 0) {
                        // sequence of characters
                        ushort extendedCharLength = 0;
                        const uint* chars = ExtendedCharTable::instance.lookupExtendedChar(c, extendedCharLength);
                        if (chars != nullptr) {
                            Q_ASSERT(extendedCharLength > 1);
                            bufferSize += extendedCharLength - 1;
                            univec.resize(bufferSize);
                            disstrU = univec.data();
                            for (int index = 0 ; index < extendedCharLength ; index++) {
                                Q_ASSERT(p < bufferSize);
                                disstrU[p++] = chars[index];
                            }
                        }
                    } else {
                        // single character
                        if (c != 0u) {
                            Q_ASSERT(p < bufferSize);
                            disstrU[p++] = c;
                        }
                    }

                    if (doubleWidth) { // assert((_image[loc(x+len,y)+1].character == 0)), see above if condition
                        len++; // Skip trailing part of multi-column character
                    }
                    len++;
                }
            }
            if ((x + len < _usedColumns) && (!_image[loc(x + len, y)].character))
                len++; // Adjust for trailing part of multi-column character

            const bool save__fixedFont = _fixedFont;
            if (lineDraw)
                _fixedFont = false;
            if (doubleWidth)
                _fixedFont = false;
            univec.resize(p);

            // Create a text scaling matrix for double width and double height lines.
            QMatrix textScale;

            if (y < _lineProperties.size()) {
                if (_lineProperties[y] & LINE_DOUBLEWIDTH)
                    textScale.scale(2, 1);

                if (_lineProperties[y] & LINE_DOUBLEHEIGHT)
                    textScale.scale(1, 2);
            }

            //Apply text scaling matrix.
            paint.setWorldMatrix(textScale, true);

            //calculate the area in which the text will be drawn
            QRect textArea = QRect(_contentRect.left() + contentsRect().left() + _fontWidth * x,
                                   _contentRect.top() + contentsRect().top() + _fontHeight * y,
                                   _fontWidth * len,
                                   _fontHeight);

            //move the calculated area to take account of scaling applied to the painter.
            //the position of the area from the origin (0,0) is scaled
            //by the opposite of whatever
            //transformation has been applied to the painter.  this ensures that
            //painting does actually start from textArea.topLeft()
            //(instead of textArea.topLeft() * painter-scale)
            textArea.moveTopLeft(textScale.inverted().map(textArea.topLeft()));

            QString unistr = QString::fromUcs4(univec.data(), univec.length());

            //paint text fragment
            if (_printerFriendly) {
                drawPrinterFriendlyTextFragment(paint,
                                                textArea,
                                                unistr,
                                                &_image[loc(x, y)]);
            } else {
                drawTextFragment(paint,
                                 textArea,
                                 unistr,
                                 &_image[loc(x, y)]);
            }

            _fixedFont = save__fixedFont;

            //reset back to single-width, single-height _lines
            paint.setWorldMatrix(textScale.inverted(), true);

            if (y < _lineProperties.size() - 1) {
                //double-height _lines are represented by two adjacent _lines
                //containing the same characters
                //both _lines will have the LINE_DOUBLEHEIGHT attribute.
                //If the current line has the LINE_DOUBLEHEIGHT attribute,
                //we can therefore skip the next line
                if (_lineProperties[y] & LINE_DOUBLEHEIGHT)
                    y++;
            }

            x += len - 1;
        }
    }
}

void TerminalDisplay::drawCurrentResultRect(QPainter& painter)
{
    if(_screenWindow->currentResultLine() == -1) {
        return;
    }

    _searchResultRect.setRect(0, _contentRect.top() + (_screenWindow->currentResultLine() - _screenWindow->currentLine()) * _fontHeight,
                              _columns * _fontWidth, _fontHeight);
    painter.fillRect(_searchResultRect, QColor(0, 0, 255, 80));
}

QRect TerminalDisplay::imageToWidget(const QRect& imageArea) const
{
    QRect result;
    result.setLeft(_contentRect.left() + _fontWidth * imageArea.left());
    result.setTop(_contentRect.top() + _fontHeight * imageArea.top());
    result.setWidth(_fontWidth * imageArea.width());
    result.setHeight(_fontHeight * imageArea.height());

    return result;
}

QRect TerminalDisplay::widgetToImage(const QRect &widgetArea) const
{
    QRect result;
    result.setLeft(  std::min(_usedColumns - 1, std::max(0, (widgetArea.left()   - contentsRect().left() - _contentRect.left()) / _fontWidth )));
    result.setTop(   std::min(_usedLines   - 1, std::max(0, (widgetArea.top()    - contentsRect().top()  - _contentRect.top() ) / _fontHeight)));
    result.setRight( std::min(_usedColumns - 1, std::max(0, (widgetArea.right()  - contentsRect().left() - _contentRect.left()) / _fontWidth )));
    result.setBottom(std::min(_usedLines   - 1, std::max(0, (widgetArea.bottom() - contentsRect().top()  - _contentRect.top() ) / _fontHeight)));
    return result;
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                          Blinking Text & Cursor                           */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::setBlinkingCursorEnabled(bool blink)
{
    _allowBlinkingCursor = blink;

    if (!hasFocus())
        return;

    if (blink && !_blinkCursorTimer->isActive())
        _blinkCursorTimer->start();

    if (!blink && _blinkCursorTimer->isActive()) {
        _blinkCursorTimer->stop();
        if (_cursorBlinking) {
            // if cursor is blinking(hidden), blink it again to make it show
            _cursorBlinking = false;
            updateCursor();
        }
        Q_ASSERT(!_cursorBlinking);
    }
}

void TerminalDisplay::setBlinkingTextEnabled(bool blink)
{
    _allowBlinkingText = blink;

    if (blink && !_blinkTextTimer->isActive())
        _blinkTextTimer->start();

    if (!blink && _blinkTextTimer->isActive()) {
        _blinkTextTimer->stop();
        _textBlinking = false;
    }
}

void TerminalDisplay::focusOutEvent(QFocusEvent*)
{
    // trigger a repaint of the cursor so that it is both:
    //
    //   * visible (in case it was hidden during blinking)
    //   * drawn in a focused out state
    _cursorBlinking = false;
    updateCursor();

    // suppress further cursor blinking
    _blinkCursorTimer->stop();
    Q_ASSERT(!_cursorBlinking);

    // if text is blinking (hidden), blink it again to make it shown
    if (_textBlinking)
        blinkTextEvent();

    // suppress further text blinking
    _blinkTextTimer->stop();
    Q_ASSERT(!_textBlinking);
}

void TerminalDisplay::focusInEvent(QFocusEvent*)
{
    if (_allowBlinkingCursor)
        _blinkCursorTimer->start();

    updateCursor();

    if (_allowBlinkingText && _hasTextBlinker)
        _blinkTextTimer->start();
}

void TerminalDisplay::blinkTextEvent()
{
    Q_ASSERT(_allowBlinkingText);

    _textBlinking = !_textBlinking;

    // TODO: Optimize to only repaint the areas of the widget where there is
    // blinking text rather than repainting the whole widget.
    update();
}

void TerminalDisplay::blinkCursorEvent()
{
    Q_ASSERT(_allowBlinkingCursor);

    _cursorBlinking = !_cursorBlinking;
    updateCursor();
}

void TerminalDisplay::updateCursor()
{
    if (!isCursorOnDisplay()){
        return;
    }

    const int cursorLocation = loc(cursorPosition().x(), cursorPosition().y());
    Q_ASSERT(cursorLocation < _imageSize);

    int charWidth = _image[cursorLocation].width();
    QRect cursorRect = imageToWidget(QRect(cursorPosition(), QSize(charWidth, 1)));
    update(cursorRect);
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                          Geometry & Resizing                              */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::resizeEvent(QResizeEvent *event)
{
    const auto width = event->size().width() - _scrollBar->geometry().width();
    _searchBar->correctPosition(QSize(width, event->size().height()));
    if (contentsRect().isValid())
        updateImageSize();
}

void TerminalDisplay::propagateSize()
{
    if (_image)
        updateImageSize();
}

void TerminalDisplay::updateImageSize()
{
    Character* oldImage = _image;
    const int oldLines = _lines;
    const int oldColumns = _columns;

    makeImage();

    if (oldImage) {
        // copy the old image to reduce flicker
        int lines = std::min(oldLines, _lines);
        int columns = std::min(oldColumns, _columns);
        for (int line = 0; line < lines; line++) {
            std::memcpy((void*)&_image[_columns * line],
                   (void*)&oldImage[oldColumns * line],
                   columns * sizeof(Character));
        }
        delete[] oldImage;
    }

    if (!_screenWindow.isNull())
        _screenWindow->setWindowLines(_lines);

    _resizing = (oldLines != _lines) || (oldColumns != _columns);

    if (_resizing) {
        showResizeNotification();
        emit changedContentSizeSignal(_contentRect.height(), _contentRect.width()); // expose resizeEvent
    }

    _resizing = false;
}

void TerminalDisplay::makeImage()
{
    _wallpaper->load();

    calcGeometry();

    // confirm that array will be of non-zero size, since the painting code
    // assumes a non-zero array length
    Q_ASSERT(_lines > 0 && _columns > 0);
    Q_ASSERT(_usedLines <= _lines && _usedColumns <= _columns);

    _imageSize = _lines * _columns;

    _image = new Character[_imageSize];

    clearImage();
}

void TerminalDisplay::clearImage()
{
    for (int i = 0; i < _imageSize; ++i)
        _image[i] = Screen::DefaultChar;
}

void TerminalDisplay::calcGeometry()
{
    _scrollBar->resize(_scrollBar->sizeHint().width(), contentsRect().height());
    _contentRect = contentsRect().adjusted(_margin, _margin, -_margin, -_margin);

    switch (_scrollbarLocation) {
    case Enum::ScrollBarHidden :
        break;
    case Enum::ScrollBarLeft :
        _contentRect.setLeft(_contentRect.left() + _scrollBar->width());
        _scrollBar->move(contentsRect().topLeft());
        break;
    case Enum::ScrollBarRight:
        _contentRect.setRight(_contentRect.right() - _scrollBar->width());
        _scrollBar->move(contentsRect().topRight() - QPoint(_scrollBar->width() - 1, 0));
        break;
    }

    // // ensure that display is always at least one column wide,
    // and clamp it to MAX_LINE_WIDTH-1 wide to prevent text shaping buffer overflows
    _columns = std::clamp(_contentRect.width() / _fontWidth, 1, MAX_LINE_WIDTH - 1);
    _usedColumns = std::min(_usedColumns, _columns);

    // ensure that display is always at least one line high
    _lines = std::max(1, _contentRect.height() / _fontHeight);
    _usedLines = std::min(_usedLines, _lines);

    if(_centerContents) {
        QSize unusedPixels = _contentRect.size() - QSize(_columns * _fontWidth, _lines * _fontHeight);
        _contentRect.adjust(unusedPixels.width() / 2, unusedPixels.height() / 2, 0, 0);
    }
}

// calculate the needed size, this must be synced with calcGeometry()
void TerminalDisplay::setSize(int columns, int lines)
{
    const int scrollBarWidth = _scrollBar->isHidden() ? 0 : _scrollBar->sizeHint().width();
    const int horizontalMargin = _margin * 2;
    const int verticalMargin = _margin * 2;

    QSize newSize = QSize(horizontalMargin + scrollBarWidth + (columns * _fontWidth)  ,
                          verticalMargin + (lines * _fontHeight));

    if (newSize != size()) {
        _size = newSize;
        updateGeometry();
    }
}

QSize TerminalDisplay::sizeHint() const
{
    return _size;
}

//showEvent and hideEvent are reimplemented here so that it appears to other classes that the
//display has been resized when the display is hidden or shown.
//
//TODO: Perhaps it would be better to have separate signals for show and hide instead of using
//the same signal as the one for a content size change
void TerminalDisplay::showEvent(QShowEvent*)
{
    emit changedContentSizeSignal(_contentRect.height(), _contentRect.width());
}
void TerminalDisplay::hideEvent(QHideEvent*)
{
    emit changedContentSizeSignal(_contentRect.height(), _contentRect.width());
}

void TerminalDisplay::setMargin(int margin)
{
    if (margin < 0) {
        margin = 0;
    }
    _margin = margin;
    updateImageSize();
}

void TerminalDisplay::setCenterContents(bool enable)
{
    _centerContents = enable;
    calcGeometry();
    update();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                Scrollbar                                  */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::setScrollBarPosition(Enum::ScrollBarPositionEnum position)
{
    if (_scrollbarLocation == position)
        return;

    if (position == Enum::ScrollBarHidden)
        _scrollBar->hide();
    else
        _scrollBar->show();

    _scrollbarLocation = position;

    propagateSize();
    update();
}

void TerminalDisplay::scrollBarPositionChanged(int)
{
    if (_screenWindow.isNull())
        return;

    _screenWindow->scrollTo(_scrollBar->value());

    // if the thumb has been moved to the bottom of the _scrollBar then set
    // the display to automatically track new output,
    // that is, scroll down automatically
    // to how new _lines as they are added
    const bool atEndOfOutput = (_scrollBar->value() == _scrollBar->maximum());
    _screenWindow->setTrackOutput(atEndOfOutput);

    updateImage();
}

void TerminalDisplay::setScroll(int cursor, int slines)
{
    // update _scrollBar if the range or value has changed,
    // otherwise return
    //
    // setting the range or value of a _scrollBar will always trigger
    // a repaint, so it should be avoided if it is not necessary
    if (_scrollBar->minimum() == 0                 &&
            _scrollBar->maximum() == (slines - _lines) &&
            _scrollBar->value()   == cursor) {
        return;
    }

    disconnect(_scrollBar, &QScrollBar::valueChanged, this, &Konsole::TerminalDisplay::scrollBarPositionChanged);
    _scrollBar->setRange(0, slines - _lines);
    _scrollBar->setSingleStep(1);
    _scrollBar->setPageStep(_lines);
    _scrollBar->setValue(cursor);
    connect(_scrollBar, &QScrollBar::valueChanged, this, &Konsole::TerminalDisplay::scrollBarPositionChanged);
}

void TerminalDisplay::setScrollFullPage(bool fullPage)
{
    _scrollFullPage = fullPage;
}

bool TerminalDisplay::scrollFullPage() const
{
    return _scrollFullPage;
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                  Mouse                                    */
/*                                                                           */
/* ------------------------------------------------------------------------- */
void TerminalDisplay::mousePressEvent(QMouseEvent* ev)
{
    if (_possibleTripleClick && (ev->button() == Qt::LeftButton)) {
        mouseTripleClickEvent(ev);
        return;
    }

    if (!contentsRect().contains(ev->pos()))
        return;

    if (_screenWindow.isNull())
        return;

    int charLine;
    int charColumn;
    getCharacterPosition(ev->pos(), charLine, charColumn, !_usesMouseTracking);
    QPoint pos = QPoint(charColumn, charLine);

    if (ev->button() == Qt::LeftButton) {
        // request the software keyboard, if any
        if (qApp->autoSipEnabled()) {
            QStyle::RequestSoftwareInputPanel behavior = QStyle::RequestSoftwareInputPanel(
                        style()->styleHint(QStyle::SH_RequestSoftwareInputPanel));
            if (hasFocus() || behavior == QStyle::RSIP_OnMouseClick) {
                QEvent event(QEvent::RequestSoftwareInputPanel);
                QApplication::sendEvent(this, &event);
            }
        }

        if (!ev->modifiers()) {
            _lineSelectionMode = false;
            _wordSelectionMode = false;
        }

        // The user clicked inside selected text
        bool selected =  _screenWindow->isSelected(pos.x(), pos.y());

        // Drag only when the Control key is held
        if ((!_ctrlRequiredForDrag || ev->modifiers() & Qt::ControlModifier) && selected) {
            _dragInfo.state = diPending;
            _dragInfo.start = ev->pos();
        } else {
            // No reason to ever start a drag event
            _dragInfo.state = diNone;

            _preserveLineBreaks = !((ev->modifiers() & Qt::ControlModifier) && !(ev->modifiers() & Qt::AltModifier));
            _columnSelectionMode = (ev->modifiers() & Qt::AltModifier) && (ev->modifiers() & Qt::ControlModifier);

    // There are a couple of use cases when selecting text :
    // Normal buffer or Alternate buffer when not using Mouse Tracking:
    //  select text or extendSelection or columnSelection or columnSelection + extendSelection
    //
    // Alternate buffer when using Mouse Tracking and with Shift pressed:
    //  select text or columnSelection
            if (!_usesMouseTracking &&
                ((ev->modifiers() == Qt::ShiftModifier) ||
                (((ev->modifiers() & Qt::ShiftModifier) != 0u) && _columnSelectionMode))) {
                    extendSelection(ev->pos());
                } else if ((!_usesMouseTracking && !((ev->modifiers() & Qt::ShiftModifier))) ||
                       (_usesMouseTracking && ((ev->modifiers() & Qt::ShiftModifier) != 0u))) {
                _screenWindow->clearSelection();

                pos.ry() += _scrollBar->value();
                _iPntSel = _pntSel = pos;
                _actSel = 1; // left mouse button pressed but nothing selected yet.
            } else if (_usesMouseTracking) {
                emit mouseSignal(0, charColumn + 1, charLine + 1 + _scrollBar->value() - _scrollBar->maximum() , 0);
            }

            if (_underlineLinks && (_openLinksByDirectClick || (ev->modifiers() & Qt::ControlModifier))) {
                Filter::HotSpot* spot = _filterChain->hotSpotAt(charLine, charColumn);
                if (spot && spot->type() == Filter::HotSpot::Link) {
                    QObject action;
                    action.setObjectName(QStringLiteral("open-action"));
                    spot->activate(&action);
                }
            }
        }
    } else if (ev->button() == Qt::MidButton) {
        processMidButtonClick(ev);
    } else if (ev->button() == Qt::RightButton) {
        if (!_usesMouseTracking || (ev->modifiers() & Qt::ShiftModifier))
            emit configureRequest(ev->pos());
        else
            emit mouseSignal(2, charColumn + 1, charLine + 1 + _scrollBar->value() - _scrollBar->maximum() , 0);
    }
}

QList<QAction*> TerminalDisplay::filterActions(const QPoint& position)
{
    int charLine, charColumn;
    getCharacterPosition(position, charLine, charColumn, false);

    Filter::HotSpot* spot = _filterChain->hotSpotAt(charLine, charColumn);

    return spot ? spot->actions() : QList<QAction*>();
}

void TerminalDisplay::mouseMoveEvent(QMouseEvent* ev)
{
    int charLine = 0;
    int charColumn = 0;
    getCharacterPosition(ev->pos(), charLine, charColumn, !_usesMouseTracking);

    processFilters();
    // handle filters
    // change link hot-spot appearance on mouse-over
    Filter::HotSpot* spot = _filterChain->hotSpotAt(charLine, charColumn);
    if (spot && spot->type() == Filter::HotSpot::Link) {
        if (_underlineLinks) {
            QRegion previousHotspotArea = _mouseOverHotspotArea;
            _mouseOverHotspotArea = QRegion();
            QRect r;
            if (spot->startLine() == spot->endLine()) {
                r.setCoords(spot->startColumn()*_fontWidth + _contentRect.left(),
                            spot->startLine()*_fontHeight + _contentRect.top(),
                            (spot->endColumn())*_fontWidth + _contentRect.left() - 1,
                            (spot->endLine() + 1)*_fontHeight + _contentRect.top() - 1);
                _mouseOverHotspotArea |= r;
            } else {
                r.setCoords(spot->startColumn()*_fontWidth + _contentRect.left(),
                            spot->startLine()*_fontHeight + _contentRect.top(),
                            (_columns)*_fontWidth + _contentRect.left() - 1,
                            (spot->startLine() + 1)*_fontHeight + _contentRect.top() - 1);
                _mouseOverHotspotArea |= r;
                for (int line = spot->startLine() + 1 ; line < spot->endLine() ; line++) {
                    r.setCoords(0 * _fontWidth + _contentRect.left(),
                                line * _fontHeight + _contentRect.top(),
                                (_columns)*_fontWidth + _contentRect.left() - 1,
                                (line + 1)*_fontHeight + _contentRect.top() - 1);
                    _mouseOverHotspotArea |= r;
                }
                r.setCoords(0 * _fontWidth + _contentRect.left(),
                            spot->endLine()*_fontHeight + _contentRect.top(),
                            (spot->endColumn())*_fontWidth + _contentRect.left() - 1,
                            (spot->endLine() + 1)*_fontHeight + _contentRect.top() - 1);
                _mouseOverHotspotArea |= r;
            }

            if ((_openLinksByDirectClick || (ev->modifiers() & Qt::ControlModifier)) && (cursor().shape() != Qt::PointingHandCursor))
                setCursor(Qt::PointingHandCursor);

            update(_mouseOverHotspotArea | previousHotspotArea);
        }
    } else if (!_mouseOverHotspotArea.isEmpty()) {
        if ((_underlineLinks && (_openLinksByDirectClick || (ev->modifiers() & Qt::ControlModifier))) || (cursor().shape() == Qt::PointingHandCursor))
            setCursor(_usesMouseTracking ? Qt::ArrowCursor : Qt::IBeamCursor);

        update(_mouseOverHotspotArea);
        // set hotspot area to an invalid rectangle
        _mouseOverHotspotArea = QRegion();
    }

    // for auto-hiding the cursor, we need mouseTracking
    if (ev->buttons() == Qt::NoButton) return;

    // if the terminal is interested in mouse movements
    // then emit a mouse movement signal, unless the shift
    // key is being held down, which overrides this.
    if (_usesMouseTracking && !(ev->modifiers() & Qt::ShiftModifier)) {
        int button = 3;
        if (ev->buttons() & Qt::LeftButton)
            button = 0;
        if (ev->buttons() & Qt::MidButton)
            button = 1;
        if (ev->buttons() & Qt::RightButton)
            button = 2;

        emit mouseSignal(button,
                         charColumn + 1,
                         charLine + 1 + _scrollBar->value() - _scrollBar->maximum(),
                         1);

        return;
    }

    if (_dragInfo.state == diPending) {
        // we had a mouse down, but haven't confirmed a drag yet
        // if the mouse has moved sufficiently, we will confirm

        const int distance = KGlobalSettings::dndEventDelay();
        if (ev->x() > _dragInfo.start.x() + distance || ev->x() < _dragInfo.start.x() - distance ||
                ev->y() > _dragInfo.start.y() + distance || ev->y() < _dragInfo.start.y() - distance) {
            // we've left the drag square, we can start a real drag operation now

            _screenWindow->clearSelection();
            doDrag();
        }
        return;
    } else if (_dragInfo.state == diDragging) {
        // this isn't technically needed because mouseMoveEvent is suppressed during
        // Qt drag operations, replaced by dragMoveEvent
        return;
    }

    if (_actSel == 0) return;

// don't extend selection while pasting
    if (ev->buttons() & Qt::MidButton) return;

    extendSelection(ev->pos());
}

void TerminalDisplay::leaveEvent(QEvent *)
{
    // remove underline from an active link when cursor leaves the widget area
    if(!_mouseOverHotspotArea.isEmpty()) {
        update(_mouseOverHotspotArea);
        _mouseOverHotspotArea = QRegion();
    }
}

void TerminalDisplay::extendSelection(const QPoint& position)
{
    if (_screenWindow.isNull())
        return;

    //if ( !contentsRect().contains(ev->pos()) ) return;
    const QPoint tL  = contentsRect().topLeft();
    const int    tLx = tL.x();
    const int    tLy = tL.y();
    const int    scroll = _scrollBar->value();

    // we're in the process of moving the mouse with the left button pressed
    // the mouse cursor will kept caught within the bounds of the text in
    // this widget.

    int linesBeyondWidget = 0;

    QRect textBounds(tLx + _contentRect.left(),
                     tLy + _contentRect.top(),
                     _usedColumns * _fontWidth - 1,
                     _usedLines * _fontHeight - 1);

    QPoint pos = position;

    // Adjust position within text area bounds.
    const QPoint oldpos = pos;

    pos.setX(bound(pos.x(), textBounds.left(), textBounds.right()));
    pos.setY(bound(pos.y(), textBounds.top(), textBounds.bottom()));

    if (oldpos.y() > textBounds.bottom()) {
        linesBeyondWidget = (oldpos.y() - textBounds.bottom()) / _fontHeight;
        _scrollBar->setValue(_scrollBar->value() + linesBeyondWidget + 1); // scrollforward
    }
    if (oldpos.y() < textBounds.top()) {
        linesBeyondWidget = (textBounds.top() - oldpos.y()) / _fontHeight;
        _scrollBar->setValue(_scrollBar->value() - linesBeyondWidget - 1); // history
    }

    int charColumn = 0;
    int charLine = 0;
    getCharacterPosition(pos, charLine, charColumn, true);

    QPoint here = QPoint(charColumn, charLine);
    QPoint ohere;
    QPoint _iPntSelCorr = _iPntSel;
    _iPntSelCorr.ry() -= _scrollBar->value();
    QPoint _pntSelCorr = _pntSel;
    _pntSelCorr.ry() -= _scrollBar->value();
    bool swapping = false;

    if (_wordSelectionMode) {
        // Extend to word boundaries
        const bool left_not_right = (here.y() < _iPntSelCorr.y() ||
                                     (here.y() == _iPntSelCorr.y() && here.x() < _iPntSelCorr.x()));
        const bool old_left_not_right = (_pntSelCorr.y() < _iPntSelCorr.y() ||
                                         (_pntSelCorr.y() == _iPntSelCorr.y() && _pntSelCorr.x() < _iPntSelCorr.x()));
        swapping = left_not_right != old_left_not_right;

        // Find left (left_not_right ? from here : from start of word)
        QPoint left = left_not_right ? here : _iPntSelCorr;

        // Find left (left_not_right ? from end of word : from here)
        QPoint right = left_not_right ? _iPntSelCorr : here;

        left = findWordStart(left);
        right = findWordEnd(right);

        // Pick which is start (ohere) and which is extension (here)
        if (left_not_right) {
            here = left;
            ohere = right;
        } else {
            here = right;
            ohere = left;
        }
        ohere.rx()++;
    }

    if (_lineSelectionMode) {
        // Extend to complete line
        const bool above_not_below = (here.y() < _iPntSelCorr.y());
        if (above_not_below) {
            ohere = findLineEnd(_iPntSelCorr);
            here = findLineStart(here);
        } else {
            ohere = findLineStart(_iPntSelCorr);
            here = findLineEnd(here);
        }

        swapping = !(_tripleSelBegin == ohere);
        _tripleSelBegin = ohere;

        ohere.rx()++;
    }

    int offset = 0;
    if (!_wordSelectionMode && !_lineSelectionMode) {
        QChar selClass;

        const bool left_not_right = (here.y() < _iPntSelCorr.y() ||
                                     (here.y() == _iPntSelCorr.y() && here.x() < _iPntSelCorr.x()));
        const bool old_left_not_right = (_pntSelCorr.y() < _iPntSelCorr.y() ||
                                         (_pntSelCorr.y() == _iPntSelCorr.y() && _pntSelCorr.x() < _iPntSelCorr.x()));
        swapping = left_not_right != old_left_not_right;

        // Find left (left_not_right ? from here : from start)
        const QPoint left = left_not_right ? here : _iPntSelCorr;

        // Find left (left_not_right ? from start : from here)
        QPoint right = left_not_right ? _iPntSelCorr : here;
        if (right.x() > 0 && !_columnSelectionMode) {
            if (right.x() - 1 < _columns && right.y() < _lines) {
                selClass = charClass(_image[loc(right.x() - 1, right.y())]);
            }
        }

        // Pick which is start (ohere) and which is extension (here)
        if (left_not_right) {
            here = left;
            ohere = right;
            offset = 0;
        } else {
            here = right;
            ohere = left;
            offset = -1;
        }
    }

    if ((here == _pntSelCorr) && (scroll == _scrollBar->value())) return; // not moved

    if (here == ohere) return; // It's not left, it's not right.

    if (_actSel < 2 || swapping) {
        if (_columnSelectionMode && !_lineSelectionMode && !_wordSelectionMode) {
            _screenWindow->setSelectionStart(ohere.x() , ohere.y() , true);
        } else {
            _screenWindow->setSelectionStart(ohere.x() - 1 - offset , ohere.y() , false);
        }
    }

    _actSel = 2; // within selection
    _pntSel = here;
    _pntSel.ry() += _scrollBar->value();

    if (_columnSelectionMode && !_lineSelectionMode && !_wordSelectionMode) {
        _screenWindow->setSelectionEnd(here.x() , here.y());
    } else {
        _screenWindow->setSelectionEnd(here.x() + offset , here.y());
    }
}

void TerminalDisplay::mouseReleaseEvent(QMouseEvent* ev)
{
    if (_screenWindow.isNull())
        return;

    int charLine;
    int charColumn;
    getCharacterPosition(ev->pos(), charLine, charColumn, !_usesMouseTracking);

    if (ev->button() == Qt::LeftButton) {
        if (_dragInfo.state == diPending) {
            // We had a drag event pending but never confirmed.  Kill selection
            _screenWindow->clearSelection();
        } else {
            if (_actSel > 1) {
                copyToX11Selection();
            }

            _actSel = 0;

            //FIXME: emits a release event even if the mouse is
            //       outside the range. The procedure used in `mouseMoveEvent'
            //       applies here, too.

            if (_usesMouseTracking && !(ev->modifiers() & Qt::ShiftModifier))
                emit mouseSignal(0,
                                 charColumn + 1,
                                 charLine + 1 + _scrollBar->value() - _scrollBar->maximum() , 2);
        }
        _dragInfo.state = diNone;
    }

    if (_usesMouseTracking &&
            (ev->button() == Qt::RightButton || ev->button() == Qt::MidButton) &&
            !(ev->modifiers() & Qt::ShiftModifier)) {
        emit mouseSignal(ev->button() == Qt::MidButton ? 1 : 2,
                         charColumn + 1,
                         charLine + 1 + _scrollBar->value() - _scrollBar->maximum() ,
                         2);
    }
}

void TerminalDisplay::getCharacterPosition(const QPoint& widgetPoint, int& line, int& column, bool edge) const
{
    // the column value returned can be equal to _usedColumns (when edge == true),
    // which is the position just after the last character displayed in a line.
    //
    // this is required so that the user can select characters in the right-most
    // column (or left-most for right-to-left input)
    const int columnMax = edge ? _usedColumns : _usedColumns - 1;
    const int xOffset = edge ? _fontWidth / 2 : 0;
    column = std::clamp((widgetPoint.x() + xOffset - contentsRect().left() - _contentRect.left()) / _fontWidth, 0, columnMax);
    line = std::clamp((widgetPoint.y() - contentsRect().top() - _contentRect.top()) / _fontHeight, 0, _usedLines - 1);
}

void TerminalDisplay::updateLineProperties()
{
    if (_screenWindow.isNull())
        return;

    _lineProperties = _screenWindow->getLineProperties();
}

void TerminalDisplay::processMidButtonClick(QMouseEvent* ev)
{
    if (!_usesMouseTracking || (ev->modifiers() & Qt::ShiftModifier)) {
        const bool appendEnter = ev->modifiers() & Qt::ControlModifier;

        if (_middleClickPasteMode == Enum::PasteFromX11Selection) {
            pasteFromX11Selection(appendEnter);
        } else if (_middleClickPasteMode == Enum::PasteFromClipboard) {
            pasteFromClipboard(appendEnter);
        } else {
            Q_ASSERT(false);
        }
    } else {
        int charLine = 0;
        int charColumn = 0;
        getCharacterPosition(ev->pos(), charLine, charColumn, !_usesMouseTracking);

        emit mouseSignal(1, charColumn + 1, charLine + 1 + _scrollBar->value() - _scrollBar->maximum() , 0);
    }
}

void TerminalDisplay::mouseDoubleClickEvent(QMouseEvent* ev)
{
    // Yes, successive middle click can trigger this event
    if (ev->button() == Qt::MidButton) {
        processMidButtonClick(ev);
        return;
    }

    if (ev->button() != Qt::LeftButton) return;
    if (_screenWindow.isNull()) return;

    int charLine = 0;
    int charColumn = 0;

    getCharacterPosition(ev->pos(), charLine, charColumn, !_usesMouseTracking);

    QPoint pos(std::min(charColumn, _columns - 1), std::min(charLine, _lines - 1));

    // pass on double click as two clicks.
    if (_usesMouseTracking && !(ev->modifiers() & Qt::ShiftModifier)) {
        // Send just _ONE_ click event, since the first click of the double click
        // was already sent by the click handler
        emit mouseSignal(0, charColumn + 1,
                         charLine + 1 + _scrollBar->value() - _scrollBar->maximum(),
                         0);  // left button
        return;
    }

    _screenWindow->clearSelection();
    _iPntSel = pos;
    _iPntSel.ry() += _scrollBar->value();

    _wordSelectionMode = true;
    _actSel = 2; // within selection

    // find word boundaries...
    {
        // find the start of the word
        const QPoint bgnSel = findWordStart(pos);
        const QPoint endSel = findWordEnd(pos);

        _actSel = 2; // within selection

        _screenWindow->setSelectionStart(bgnSel.x() , bgnSel.y() , false);
        _screenWindow->setSelectionEnd(endSel.x() , endSel.y());

        copyToX11Selection();
    }

    _possibleTripleClick = true;

    QTimer::singleShot(QApplication::doubleClickInterval(), this, [this]() {
        _possibleTripleClick = false;
    });
}

void TerminalDisplay::wheelEvent(QWheelEvent* ev)
{
    // Only vertical scrolling is supported
    if (ev->orientation() != Qt::Vertical)
        return;

    const int modifiers = ev->modifiers();
    const int delta = ev->delta();

    // ctrl+<wheel> for zooming, like in konqueror and firefox
    if ((modifiers & Qt::ControlModifier) && _mouseWheelZoom) {
        if (delta > 0) {
            // wheel-up for increasing font size
            increaseFontSize();
        } else {
            // wheel-down for decreasing font size
            decreaseFontSize();
        }

        return;
    }

    // If the program running in the terminal is not interested in mouse events:
    //  - Send the event to the scrollbar if the slider has room to move
    //  - Otherwise, send simulated up / down key presses to the terminal program
    //    for the benefit of programs such as 'less' (which use the alternate screen)
    if (!_usesMouseTracking && _alternateScrolling) {
        Q_ASSERT(_sessionController != nullptr);

        const bool canScroll = _scrollBar->maximum() > 0;
        if (canScroll) {
            _scrollBar->event(ev);
            _sessionController->setSearchStartToWindowCurrentLine();
         } else {
            Q_ASSERT(!_sessionController->session().isNull());
             if (!_sessionController->session()->isPrimaryScreen()) {
                // assume that each Up / Down key event will cause the terminal application
                // to scroll by one line.
                //
                // to get a reasonable scrolling speed, scroll by one line for every 5 degrees
                // of mouse wheel rotation.  Mouse wheels typically move in steps of 15 degrees,
                // giving a scroll of 3 lines
                const int keyCode = delta > 0 ? Qt::Key_Up : Qt::Key_Down;
                QKeyEvent keyEvent(QEvent::KeyPress, keyCode, Qt::NoModifier);

                // QWheelEvent::delta() gives rotation in eighths of a degree
                const int degrees = delta / 8;
                const int lines = abs(degrees) / 5;

                for (int i = 0; i < lines; i++)
                    emit keyPressedSignal(&keyEvent);
             }
        }
    } else {
        // terminal program wants notification of mouse activity

        int charLine;
        int charColumn;
        getCharacterPosition(ev->pos() , charLine , charColumn, !_usesMouseTracking);

        emit mouseSignal(delta > 0 ? 4 : 5,
                         charColumn + 1,
                         charLine + 1 + _scrollBar->value() - _scrollBar->maximum() ,
                         0);
    }
}

void TerminalDisplay::viewScrolledByUser()
{
    Q_ASSERT(_sessionController != nullptr);
    _sessionController->setSearchStartToWindowCurrentLine();
}

/* Moving left/up from the line containing pnt, return the starting 
   offset point which the given line is continuously wrapped
   (top left corner = 0,0; previous line not visible = 0,-1).
*/
QPoint TerminalDisplay::findLineStart(const QPoint &pnt)
{
    const int visibleScreenLines = _lineProperties.size();
    const int topVisibleLine = _screenWindow->currentLine();
    Screen *screen = _screenWindow->screen();
    int line = pnt.y();
    int lineInHistory= line + topVisibleLine;

    QVector<LineProperty> lineProperties = _lineProperties;

    while (lineInHistory > 0) {
        for (; line > 0; line--, lineInHistory--) {
            // Does previous line wrap around?
            if (!(lineProperties[line - 1] & LINE_WRAPPED)) {
                return QPoint(0, lineInHistory - topVisibleLine);
            }
        }

        if (lineInHistory < 1)
            break;

        // _lineProperties is only for the visible screen, so grab new data
        int newRegionStart = std::max(0, lineInHistory - visibleScreenLines);
        lineProperties = screen->getLineProperties(newRegionStart, lineInHistory - 1);
        line = lineInHistory - newRegionStart;
    }
    return QPoint(0, lineInHistory - topVisibleLine);
}

/* Moving right/down from the line containing pnt, return the ending 
   offset point which the given line is continuously wrapped.
*/
QPoint TerminalDisplay::findLineEnd(const QPoint &pnt)
{
    const int visibleScreenLines = _lineProperties.size();
    const int topVisibleLine = _screenWindow->currentLine();
    const int maxY = _screenWindow->lineCount() - 1;
    Screen *screen = _screenWindow->screen();
    int line = pnt.y();
    int lineInHistory= line + topVisibleLine;

    QVector<LineProperty> lineProperties = _lineProperties;

    while (lineInHistory < maxY) {
        for (; line < lineProperties.count() && lineInHistory < maxY; line++, lineInHistory++) {
            // Does current line wrap around?
            if (!(lineProperties[line] & LINE_WRAPPED)) {
                return QPoint(_columns - 1, lineInHistory - topVisibleLine);
            }
        }

        line = 0;
        lineProperties = screen->getLineProperties(lineInHistory, std::min(lineInHistory + visibleScreenLines, maxY));
    }
    return QPoint(_columns - 1, lineInHistory - topVisibleLine);
}

QPoint TerminalDisplay::findWordStart(const QPoint &pnt)
{
    const int regSize = std::max(_screenWindow->windowLines(), 10);
    const int firstVisibleLine = _screenWindow->currentLine();

    Screen *screen = _screenWindow->screen();
    Character *image = _image;
    Character *tmp_image = nullptr;

    int imgLine = pnt.y();
    int x = pnt.x();
    int y = imgLine + firstVisibleLine;
    int imgLoc = loc(x, imgLine);
    QVector<LineProperty> lineProperties = _lineProperties;
    const QChar selClass = charClass(image[imgLoc]);
    const int imageSize = regSize * _columns;

    while (true) {
        for (;;imgLoc--, x--) {
            if (imgLoc < 1) {
                // no more chars in this region
                break;
            }
            if (x > 0) {
                // has previous char on this line
                if (charClass(image[imgLoc - 1]) == selClass)
                    continue;
                goto out;
            } else if (imgLine > 0) {
                // not the first line in the session
                if ((lineProperties[imgLine - 1] & LINE_WRAPPED) != 0) {
                    // have continuation on prev line
                    if (charClass(image[imgLoc - 1]) == selClass) {
                        x = _columns;
                        imgLine--;
                        y--;
                        continue;
                    }
                }
                goto out;
            } else if (y > 0) {
                // want more data, but need to fetch new region
                break;
            } else {
                goto out;
            }
        }
        if (y <= 0) {
            // No more data
            goto out;
        }
        int newRegStart = std::max(0, y - regSize + 1);
        lineProperties = screen->getLineProperties(newRegStart, y - 1);
        imgLine = y - newRegStart;

        delete[] tmp_image;
        tmp_image = new Character[imageSize];
        image = tmp_image;

        screen->getImage(tmp_image, imageSize, newRegStart, y - 1);
        imgLoc = loc(x, imgLine);
        if (imgLoc < 1) {
            // Reached the start of the session
            break;
        }
    }
out:
    delete[] tmp_image;

    return QPoint(x, y - firstVisibleLine);
}

QPoint TerminalDisplay::findWordEnd(const QPoint &pnt)
{
    const int regSize = std::max(_screenWindow->windowLines(), 10);
    const int curLine = _screenWindow->currentLine();
    int i = pnt.y();
    int x = pnt.x();
    int y = i + curLine;
    int j = loc(x, i);
    QVector<LineProperty> lineProperties = _lineProperties;
    Screen *screen = _screenWindow->screen();
    Character *image = _image;
    Character *tmp_image = nullptr;
    const QChar selClass = charClass(image[j]);
    const int imageSize = regSize * _columns;
    const int maxY = _screenWindow->lineCount() - 1;
    const int maxX = _columns - 1;

    while (true) {
        const int lineCount = lineProperties.count();
        for (;;j++, x++) {
            if (x < maxX) {
                if (charClass(image[j + 1]) == selClass &&
                    // A colon right before whitespace is never part of a word
                    ! (image[j + 1].character == ':' && charClass(image[j + 2]) == QLatin1Char(' '))) {
                    continue;
                }
                goto out;
            } else if (i < lineCount - 1) {
                if (lineProperties[i] & LINE_WRAPPED &&
                    charClass(image[j + 1]) == selClass &&
                    // A colon right before whitespace is never part of a word
                    ! (image[j + 1].character == ':' && charClass(image[j + 2]) == QLatin1Char(' '))) {
                    x = -1;
                    i++;
                    y++;
                    continue;
                }
                goto out;
            } else if (y < maxY) {
                if (i < lineCount && !(lineProperties[i] & LINE_WRAPPED))
                    goto out;
                break;
            } else {
                goto out;
            }
        }
        int newRegEnd = std::min(y + regSize - 1, maxY);
        lineProperties = screen->getLineProperties(y, newRegEnd);
        i = 0;
        if (!tmp_image) {
            tmp_image = new Character[imageSize];
            image = tmp_image;
        }
        screen->getImage(tmp_image, imageSize, y, newRegEnd);
        x--;
        j = loc(x, i);
    }
out:
    y -= curLine;
    // In word selection mode don't select @ (64) if at end of word.
    if (((image[j].rendition & RE_EXTENDED_CHAR) == 0) &&
        (QChar(image[j].character) == QLatin1Char('@')) &&
        (y > pnt.y() || x > pnt.x())) {
        if (x > 0) {
            x--;
        } else {
            y--;
        }
    }
    delete[] tmp_image;

    return QPoint(x, y);
}

Screen::DecodingOptions TerminalDisplay::currentDecodingOptions()
{
    Screen::DecodingOptions decodingOptions;
    if (_preserveLineBreaks) {
        decodingOptions |= Screen::PreserveLineBreaks;
    }
    if (_trimLeadingSpaces) {
        decodingOptions |= Screen::TrimLeadingWhitespace;
    }
    if (_trimTrailingSpaces) {
        decodingOptions |= Screen::TrimTrailingWhitespace;
    }

    return decodingOptions;
}

void TerminalDisplay::mouseTripleClickEvent(QMouseEvent* ev)
{
    if (_screenWindow.isNull()) return;

    int charLine;
    int charColumn;
    getCharacterPosition(ev->pos(), charLine, charColumn, true);
    selectLine(QPoint(charColumn, charLine),
               _tripleClickMode == Enum::SelectWholeLine);
}

void TerminalDisplay::selectLine(QPoint pos, bool entireLine)
{
    _iPntSel = pos;

    _screenWindow->clearSelection();

    _lineSelectionMode = true;
    _wordSelectionMode = false;

    _actSel = 2; // within selection

    if (!entireLine) { // Select from cursor to end of line
        _tripleSelBegin = findWordStart(_iPntSel);
        _screenWindow->setSelectionStart(_tripleSelBegin.x(),
                                         _tripleSelBegin.y() , false);
    } else {
        _tripleSelBegin = findLineStart(_iPntSel);
        _screenWindow->setSelectionStart(0 , _tripleSelBegin.y() , false);
    }

    _iPntSel = findLineEnd(_iPntSel);
    _screenWindow->setSelectionEnd(_iPntSel.x() , _iPntSel.y());

    copyToX11Selection();

    _iPntSel.ry() += _scrollBar->value();
}

void TerminalDisplay::selectCurrentLine()
{
    if (_screenWindow.isNull()) return;

    selectLine(cursorPosition(), true);
}

void TerminalDisplay::selectAll()
{
    if (_screenWindow.isNull()) {
        return;
    }

    _preserveLineBreaks = true;
    _screenWindow->setSelectionByLineRange(0, _screenWindow->lineCount());
    copyToX11Selection();
}

bool TerminalDisplay::focusNextPrevChild(bool next)
{
    // for 'Tab', always disable focus switching among widgets
    // for 'Shift+Tab', leave the decision to higher level
    if (next)
        return false;
    else
        return QWidget::focusNextPrevChild(next);
}

QChar TerminalDisplay::charClass(const Character& ch) const
{
    if (ch.rendition & RE_EXTENDED_CHAR) {
        ushort extendedCharLength = 0;
        const uint* chars = ExtendedCharTable::instance.lookupExtendedChar(ch.character, extendedCharLength);
        if (chars && extendedCharLength > 0) {
            const QString s = QString::fromUcs4(chars, extendedCharLength);
            if (_wordCharacters.contains(s, Qt::CaseInsensitive))
                return QLatin1Char('a');
            bool letterOrNumber = false;
            for (int i = 0; !letterOrNumber && i < s.size(); ++i) {
                letterOrNumber = s.at(i).isLetterOrNumber();
            }
            return letterOrNumber ? QLatin1Char('a') : s.at(0);
        }
        return 0;
    } else {
        const QChar qch(ch.character);
        if (qch.isSpace()) return QLatin1Char(' ');

        if (qch.isLetterOrNumber() || _wordCharacters.contains(qch, Qt::CaseInsensitive))
            return QLatin1Char('a');

        return qch;
    }
}

void TerminalDisplay::setWordCharacters(const QString& wc)
{
    _wordCharacters = wc;
}

void TerminalDisplay::setUsesMouseTracking(bool on)
{
    _usesMouseTracking = on;
    setCursor(_usesMouseTracking ? Qt::ArrowCursor : Qt::IBeamCursor);
}
bool TerminalDisplay::usesMouseTracking() const
{
    return _usesMouseTracking;
}

void TerminalDisplay::setAlternateScrolling(bool enable)
{
    _alternateScrolling = enable;
}
bool TerminalDisplay::alternateScrolling() const
{
    return _alternateScrolling;
}

void TerminalDisplay::setBracketedPasteMode(bool on)
{
    _bracketedPasteMode = on;
}
bool TerminalDisplay::bracketedPasteMode() const
{
    return _bracketedPasteMode;
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                               Clipboard                                   */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::doPaste(QString text, bool appendReturn)
{
    if (_screenWindow.isNull())
        return;

    if (appendReturn)
        text.append(QLatin1String("\r"));

    if (text.length() > 8000) {
        if (KMessageBox::warningContinueCancel(window(),
                        i18np("Are you sure you want to paste %1 character?",
                              "Are you sure you want to paste %1 characters?",
                              text.length()),
                        i18n("Confirm Paste"),
                        KStandardGuiItem::cont(),
                        KStandardGuiItem::cancel(),
                        QStringLiteral("ShowPasteHugeTextWarning")) == KMessageBox::Cancel)
            return;
    }

    if (!text.isEmpty()) {
        text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
        if (bracketedPasteMode()) {
            text.remove(QLatin1String("\033"));
            text.prepend(QLatin1String("\033[200~"));
            text.append(QLatin1String("\033[201~"));
        }
        // perform paste by simulating keypress events
        QKeyEvent e(QEvent::KeyPress, 0, Qt::NoModifier, text);
        emit keyPressedSignal(&e);
    }
}

void TerminalDisplay::setAutoCopySelectedText(bool enabled)
{
    _autoCopySelectedText = enabled;
}

void TerminalDisplay::setMiddleClickPasteMode(Enum::MiddleClickPasteModeEnum mode)
{
    _middleClickPasteMode = mode;
}

void TerminalDisplay::copyToX11Selection()
{
    if (_screenWindow.isNull())
        return;

    QString text = _screenWindow->selectedText(currentDecodingOptions());
    if (text.isEmpty())
        return;

    if (QApplication::clipboard()->supportsSelection()) {
        QApplication::clipboard()->setText(text, QClipboard::Selection);
    }

    if (_autoCopySelectedText)
        QApplication::clipboard()->setText(text, QClipboard::Clipboard);
}

void TerminalDisplay::copyToClipboard()
{
    if (_screenWindow.isNull())
        return;

    QString text = _screenWindow->selectedText(currentDecodingOptions());
    if (text.isEmpty())
        return;

    QApplication::clipboard()->setText(text, QClipboard::Clipboard);
}

void TerminalDisplay::pasteFromClipboard(bool appendEnter)
{
    QString text = QApplication::clipboard()->text(QClipboard::Clipboard);
    doPaste(text, appendEnter);
}

void TerminalDisplay::pasteFromX11Selection(bool appendEnter)
{
    if (QApplication::clipboard()->supportsSelection()) {
        QString text = QApplication::clipboard()->text(QClipboard::Selection);
        doPaste(text, appendEnter);
    }
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                Input Method                               */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::inputMethodEvent(QInputMethodEvent* event)
{
    if (!event->commitString().isEmpty()) {
        QKeyEvent keyEvent(QEvent::KeyPress, 0, Qt::NoModifier, event->commitString());
        emit keyPressedSignal(&keyEvent);
    }

    if (isCursorOnDisplay()) {
        _inputMethodData.preeditString = event->preeditString();
        update(preeditRect() | _inputMethodData.previousPreeditRect);
    }

    event->accept();
}

QVariant TerminalDisplay::inputMethodQuery(Qt::InputMethodQuery query) const
{
    const QPoint cursorPos = cursorPosition();
    switch (query) {
    case Qt::ImMicroFocus:
        return imageToWidget(QRect(cursorPos.x(), cursorPos.y(), 1, 1));
    case Qt::ImFont:
        return font();
    case Qt::ImCursorPosition:
        // return the cursor position within the current line
        return cursorPos.x();
    case Qt::ImSurroundingText: {
        // return the text from the current line
        QString lineText;
        QTextStream stream(&lineText);
        PlainTextDecoder decoder;
        decoder.begin(&stream);
        if (isCursorOnDisplay()) {
            decoder.decodeLine(&_image[loc(0, cursorPos.y())], _usedColumns, LINE_DEFAULT);
        }
        decoder.end();
        return lineText;
    }
    case Qt::ImCurrentSelection:
        return QString();
    default:
        break;
    }

    return QVariant();
}

QRect TerminalDisplay::preeditRect() const
{
    const int preeditLength = Character::stringWidth(_inputMethodData.preeditString);

    if (preeditLength == 0)
        return QRect();
    const QRect stringRect(_contentRect.left() + _fontWidth * cursorPosition().x(),
                           _contentRect.top() + _fontHeight * cursorPosition().y(),
                           _fontWidth * preeditLength,
                           _fontHeight);

    return stringRect.intersected(_contentRect);
}

void TerminalDisplay::drawInputMethodPreeditString(QPainter& painter , const QRect& rect)
{
    if (_inputMethodData.preeditString.isEmpty() || !isCursorOnDisplay())
        return;

    const QPoint cursorPos = cursorPosition();

    bool invertColors = false;
    const QColor background = _colorTable[DEFAULT_BACK_COLOR];
    const QColor foreground = _colorTable[DEFAULT_FORE_COLOR];
    const Character* style = &_image[loc(cursorPos.x(), cursorPos.y())];

    drawBackground(painter, rect, background, true);
    drawCursor(painter, rect, foreground, background, invertColors);
    drawCharacters(painter, rect, _inputMethodData.preeditString, style, invertColors);

    _inputMethodData.previousPreeditRect = rect;
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                Keyboard                                   */
/*                                                                           */
/* ------------------------------------------------------------------------- */

void TerminalDisplay::setFlowControlWarningEnabled(bool enable)
{
    _flowControlWarningEnabled = enable;

    // if the dialog is currently visible and the flow control warning has
    // been disabled then hide the dialog
    if (!enable)
        outputSuspended(false);
}

void TerminalDisplay::outputSuspended(bool suspended)
{
    //create the label when this function is first called
    if (!_outputSuspendedLabel) {
        //This label includes a link to an English language website
        //describing the 'flow control' (Xon/Xoff) feature found in almost
        //all terminal emulators.
        //If there isn't a suitable article available in the target language the link
        //can simply be removed.
        _outputSuspendedLabel = new QLabel(i18n("<qt>Output has been "
                                                "<a href=\"http://en.wikipedia.org/wiki/Software_flow_control\">suspended</a>"
                                                " by pressing Ctrl+S."
                                                "  Press <b>Ctrl+Q</b> to resume."
                                                "  This message will be dismissed in 10 seconds.</qt>"),
                                           this);

        QPalette palette(_outputSuspendedLabel->palette());
        KColorScheme::adjustBackground(palette, KColorScheme::NeutralBackground);
        _outputSuspendedLabel->setPalette(palette);
        _outputSuspendedLabel->setAutoFillBackground(true);
        _outputSuspendedLabel->setBackgroundRole(QPalette::Base);
        _outputSuspendedLabel->setFont(KGlobalSettings::smallestReadableFont());
        _outputSuspendedLabel->setContentsMargins(5, 5, 5, 5);
        _outputSuspendedLabel->setWordWrap(true);

        //enable activation of "Xon/Xoff" link in label
        _outputSuspendedLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
                Qt::LinksAccessibleByKeyboard);
        _outputSuspendedLabel->setOpenExternalLinks(true);
        _outputSuspendedLabel->setVisible(false);

        _gridLayout->addWidget(_outputSuspendedLabel);
        _gridLayout->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding,
                                             QSizePolicy::Expanding),
                             1, 0);
    }
    // Remove message after a few seconds
    if (suspended) {
        QTimer::singleShot(10000, this, SLOT(dismissOutputSuspendedMessage()));
    }

    _outputSuspendedLabel->setVisible(suspended);
}

void TerminalDisplay::dismissOutputSuspendedMessage()
{
    outputSuspended(false);
}

void TerminalDisplay::scrollScreenWindow(enum ScreenWindow::RelativeScrollMode mode, int amount)
{
    _screenWindow->scrollBy(mode, amount, _scrollFullPage);
    _screenWindow->setTrackOutput(_screenWindow->atEndOfOutput());
    updateLineProperties();
    updateImage();
    viewScrolledByUser();
}

void TerminalDisplay::keyPressEvent(QKeyEvent* event)
{
    _screenWindow->screen()->setCurrentTerminalDisplay(this);

    _actSel = 0; // Key stroke implies a screen update, so TerminalDisplay won't
    // know where the current selection is.

    if (_allowBlinkingCursor) {
        _blinkCursorTimer->start();
        if (_cursorBlinking) {
            // if cursor is blinking(hidden), blink it again to show it
            blinkCursorEvent();
        }
        Q_ASSERT(!_cursorBlinking);
    }

    if (_searchBar->isVisible() && (event->key() == Qt::Key_Escape)) {
        _sessionController->searchClosed();
    }

    emit keyPressedSignal(event);

#ifndef QT_NO_ACCESSIBILITY
    QAccessible::updateAccessibility(this, 0, QAccessible::TextCaretMoved);
#endif

    event->accept();
}

bool TerminalDisplay::handleShortcutOverrideEvent(QKeyEvent* keyEvent)
{
    const int modifiers = keyEvent->modifiers();

    //  When a possible shortcut combination is pressed,
    //  emit the overrideShortcutCheck() signal to allow the host
    //  to decide whether the terminal should override it or not.
    if (modifiers != Qt::NoModifier) {
        int modifierCount = 0;
        unsigned int currentModifier = Qt::ShiftModifier;

        while (currentModifier <= Qt::KeypadModifier) {
            if (modifiers & currentModifier)
                modifierCount++;
            currentModifier <<= 1;
        }
        if (modifierCount < 2) {
            bool override = false;
            emit overrideShortcutCheck(keyEvent, override);
            if (override) {
                keyEvent->accept();
                return true;
            }
        }
    }

    // Override any of the following shortcuts because
    // they are needed by the terminal
    int keyCode = keyEvent->key() | modifiers;
    switch (keyCode) {
        // list is taken from the QLineEdit::event() code
    case Qt::Key_Tab:
    case Qt::Key_Delete:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_Backspace:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Slash:
    case Qt::Key_Period:
    case Qt::Key_Space:
        keyEvent->accept();
        return true;
    }
    return false;
}

bool TerminalDisplay::event(QEvent* event)
{
    bool eventHandled = false;
    switch (event->type()) {
    case QEvent::ShortcutOverride:
        eventHandled = handleShortcutOverrideEvent(static_cast<QKeyEvent*>(event));
        break;
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
        _scrollBar->setPalette(QApplication::palette());
        break;
    default:
        break;
    }
    return eventHandled ? true : QWidget::event(event);
}

void TerminalDisplay::contextMenuEvent(QContextMenuEvent* event)
{
    // the logic for the mouse case is within MousePressEvent()
    if (event->reason() != QContextMenuEvent::Mouse) {
        emit configureRequest(mapFromGlobal(QCursor::pos()));
    }
}

/* --------------------------------------------------------------------- */
/*                                                                       */
/*                                  Bell                                 */
/*                                                                       */
/* --------------------------------------------------------------------- */

void TerminalDisplay::setBellMode(int mode)
{
    _bellMode = mode;
}

int TerminalDisplay::bellMode() const
{
    return _bellMode;
}

void TerminalDisplay::bell(const QString& message)
{
    if (_bellMasked)
        return;

    switch (_bellMode) {
    case Enum::SystemBeepBell:
        KNotification::beep();
        break;
    case Enum::NotifyBell:
        // STABLE API:
        //     Please note that these event names, "BellVisible" and "BellInvisible",
        //     should not change and should be kept stable, because other applications
        //     that use this code via KPart rely on these names for notifications.
        KNotification::event(hasFocus() ? QStringLiteral("BellVisible") : QStringLiteral("BellInvisible"),
                             message, QPixmap(), this);
        break;
    case Enum::VisualBell:
        visualBell();
        break;
    default:
        break;
    }

    // limit the rate at which bells can occur.
    // ...mainly for sound effects where rapid bells in sequence
    // produce a horrible noise.
    _bellMasked = true;
    QTimer::singleShot(500, this, [this]() {
        _bellMasked = false;
    });
}

void TerminalDisplay::visualBell()
{
    swapFGBGColors();
    QTimer::singleShot(200, this, &Konsole::TerminalDisplay::swapFGBGColors);
}

void TerminalDisplay::swapFGBGColors()
{
    // swap the default foreground & background color
    ColorEntry color = _colorTable[DEFAULT_BACK_COLOR];
    _colorTable[DEFAULT_BACK_COLOR] = _colorTable[DEFAULT_FORE_COLOR];
    _colorTable[DEFAULT_FORE_COLOR] = color;

    update();
}

/* --------------------------------------------------------------------- */
/*                                                                       */
/* Drag & Drop                                                           */
/*                                                                       */
/* --------------------------------------------------------------------- */

void TerminalDisplay::dragEnterEvent(QDragEnterEvent* event)
{
    // text/plain alone is enough for KDE-apps
    // text/uri-list is for supporting some non-KDE apps, such as thunar
    //   and pcmanfm
    // That also applies in dropEvent()
    const auto mimeData = event->mimeData();
    if (mimeData
            && (mimeData->hasFormat(QStringLiteral("text/plain"))
                || mimeData->hasFormat(QStringLiteral("text/uri-list")))) {
        event->acceptProposedAction();
    }
}

void TerminalDisplay::dropEvent(QDropEvent* event)
{
    const auto mimeData = event->mimeData();
    if (!mimeData) {
        return;
    }
    KUrl::List urls = KUrl::List::fromMimeData(mimeData);

    QString dropText;
    if (!urls.isEmpty()) {
        for (int i = 0 ; i < urls.count() ; i++) {
            KUrl url = KIO::NetAccess::mostLocalUrl(urls[i] , 0);
            QString urlText;

            if (url.isLocalFile())
                urlText = url.path();
            else
                urlText = url.url();

            // in future it may be useful to be able to insert file names with drag-and-drop
            // without quoting them (this only affects paths with spaces in)
            urlText = KShell::quoteArg(urlText);

            dropText += urlText;

            // Each filename(including the last) should be followed by one space.
            dropText += QLatin1Char(' ');
        }

#if defined(HAVE_LIBKONQ)
        // If our target is local we will open a popup - otherwise the fallback kicks
        // in and the URLs will simply be pasted as text.
        if (_sessionController && _sessionController->url().isLocalFile()) {
            // A standard popup with Copy, Move and Link as options -
            // plus an additional Paste option.

            QAction* pasteAction = new QAction(i18n("&Paste Location"), this);
            pasteAction->setData(dropText);
            connect(pasteAction, SIGNAL(triggered()), this, SLOT(dropMenuPasteActionTriggered()));

            QList<QAction*> additionalActions;
            additionalActions.append(pasteAction);

            if (urls.count() == 1) {
                const KUrl url = KIO::NetAccess::mostLocalUrl(urls[0] , 0);

                if (url.isLocalFile()) {
                    const QFileInfo fileInfo(url.path());

                    if (fileInfo.isDir()) {
                        QAction* cdAction = new QAction(i18n("Change &Directory To"), this);
                        dropText = QLatin1String(" cd ") + dropText + QLatin1Char('\n');
                        cdAction->setData(dropText);
                        connect(cdAction, SIGNAL(triggered()), this, SLOT(dropMenuCdActionTriggered()));
                        additionalActions.append(cdAction);
                    }
                }
            }

            KUrl target(_sessionController->currentDir());

            KonqOperations::doDrop(KFileItem(), target, event, this, additionalActions);

            return;
        }
#endif

    } else {
        dropText = mimeData->text();
    }

    if (mimeData->hasFormat(QStringLiteral("text/plain")) ||
            mimeData->hasFormat(QStringLiteral("text/uri-list"))) {
        emit sendStringToEmu(qPrintable(dropText));
    }
}

void TerminalDisplay::dropMenuPasteActionTriggered()
{
    if (sender()) {
        const QAction* action = qobject_cast<const QAction*>(sender());
        if (action) {
            emit sendStringToEmu(qPrintable(action->data().toString()));
        }
    }
}

void TerminalDisplay::dropMenuCdActionTriggered()
{
    if (sender()) {
        const QAction* action = qobject_cast<const QAction*>(sender());
        if (action) {
            emit sendStringToEmu(qPrintable(action->data().toString()));
        }
    }
}

void TerminalDisplay::doDrag()
{
    const QMimeData *clipboardMimeData = QApplication::clipboard()->mimeData(QClipboard::Selection);
    if (!clipboardMimeData) {
        return;
    }
    auto mimeData = new QMimeData();
    _dragInfo.state = diDragging;
    _dragInfo.dragObject = new QDrag(this);
    mimeData->setText(clipboardMimeData->text());
    _dragInfo.dragObject->setMimeData(mimeData);
    _dragInfo.dragObject->exec(Qt::CopyAction);
}

void TerminalDisplay::setSessionController(SessionController* controller)
{
    _sessionController = controller;
}

SessionController* TerminalDisplay::sessionController()
{
    return _sessionController;
}

IncrementalSearchBar *TerminalDisplay::searchBar() const
{
    return _searchBar;
}

AutoScrollHandler::AutoScrollHandler(QWidget* parent)
    : QObject(parent)
    , _timerId(0)
{
    parent->installEventFilter(this);
}
void AutoScrollHandler::timerEvent(QTimerEvent* event)
{
    if (event->timerId() != _timerId)
        return;

    QMouseEvent mouseEvent(QEvent::MouseMove,
                           widget()->mapFromGlobal(QCursor::pos()),
                           Qt::NoButton,
                           Qt::LeftButton,
                           Qt::NoModifier);

    QApplication::sendEvent(widget(), &mouseEvent);
}
bool AutoScrollHandler::eventFilter(QObject* watched, QEvent* event)
{
    Q_ASSERT(watched == parent());
    Q_UNUSED(watched);

    switch (event->type()) {
    case QEvent::MouseMove: {
        QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
        bool mouseInWidget = widget()->rect().contains(mouseEvent->pos());
        if (mouseInWidget) {
            if (_timerId)
                killTimer(_timerId);

            _timerId = 0;
        } else {
            if (!_timerId && (mouseEvent->buttons() & Qt::LeftButton))
                _timerId = startTimer(100);
        }

        break;
    }
    case QEvent::MouseButtonRelease: {
        QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
        if (_timerId && (mouseEvent->buttons() & ~Qt::LeftButton)) {
            killTimer(_timerId);
            _timerId = 0;
        }
        break;
    }
    default:
        break;
    };

    return false;
}

void TerminalDisplay::applyProfile(const Profile::Ptr &profile)
{
    // load color scheme
    ColorEntry table[TABLE_COLORS];
    _colorScheme = ViewManager::colorSchemeForProfile(profile);
    _colorScheme->getColorTable(table, sessionController()->session()->sessionId());
    setColorTable(table);
    setOpacity(_colorScheme->opacity());
    setWallpaper(_colorScheme->wallpaper());

    // load font
    _antialiasText = profile->antiAliasFonts();
    _useFontLineCharacters = profile->useFontLineCharacters();
    _boldIntense = profile->boldIntense();
    setVTFont(profile->font());

    // set scroll-bar position
    setScrollBarPosition(Enum::ScrollBarPositionEnum(profile->property<int>(Profile::ScrollBarPosition)));
    setScrollFullPage(profile->property<bool>(Profile::ScrollFullPage));

    // show hint about terminal size after resizing
    _showTerminalSizeHint = profile->showTerminalSizeHint();

    // terminal features
    setBlinkingCursorEnabled(profile->blinkingCursorEnabled());
    setBlinkingTextEnabled(profile->blinkingTextEnabled());

    _tripleClickMode = Enum::TripleClickModeEnum(profile->property<int>(Profile::TripleClickMode));

    setAutoCopySelectedText(profile->autoCopySelectedText());
    setUnderlineLinks(profile->underlineLinksEnabled());
    _ctrlRequiredForDrag = profile->property<bool>(Profile::CtrlRequiredForDrag);
    _bidiEnabled = profile->bidiRenderingEnabled();
    setLineSpacing(profile->lineSpacing());
    _trimLeadingSpaces = profile->property<bool>(Profile::TrimLeadingSpacesInSelectedText);
    _trimTrailingSpaces = profile->property<bool>(Profile::TrimTrailingSpacesInSelectedText);
    _openLinksByDirectClick = profile->property<bool>(Profile::OpenLinksByDirectClickEnabled);

    setMiddleClickPasteMode(Enum::MiddleClickPasteModeEnum(profile->property<int>(Profile::MiddleClickPasteMode)));

    // margin/center
    setMargin(profile->property<int>(Profile::TerminalMargin));
    setCenterContents(profile->property<bool>(Profile::TerminalCenter));

    // cursor shape
    setKeyboardCursorShape(Enum::CursorShapeEnum(profile->property<int>(Profile::CursorShape)));

    // cursor color
    // an invalid QColor is used to inform the view widget to
    // draw the cursor using the default color( matching the text)
    setKeyboardCursorColor(profile->useCustomCursorColor() ? profile->customCursorColor() : QColor());

    // word characters
    setWordCharacters(profile->wordCharacters());

    // bell mode
    setBellMode(profile->property<int>(Profile::BellMode));

    // mouse wheel zoom
    _mouseWheelZoom = profile->mouseWheelZoomEnabled() ;
    setAlternateScrolling(profile->property<bool>(Profile::AlternateScrolling));
}

#include "TerminalDisplay.moc"
