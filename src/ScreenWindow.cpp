/*
    Copyright (C) 2007 by Robert Knight <robertknight@gmail.com>

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
#include "ScreenWindow.h"

// Konsole
#include "Screen.h"

using namespace Konsole;

ScreenWindow::ScreenWindow(Screen* screen, QObject* parent)
    : QObject(parent)
    , _windowBuffer(0)
    , _windowBufferSize(0)
    , _bufferNeedsUpdate(true)
    , _windowLines(1)
    , _currentLine(0)
    , _currentResultLine(-1)
    , _trackOutput(true)
    , _scrollCount(0)
{
    setScreen(screen);
}

ScreenWindow::~ScreenWindow()
{
    delete[] _windowBuffer;
}
void ScreenWindow::setScreen(Screen* screen)
{
    Q_ASSERT(screen);

    _screen = screen;
}

Screen* ScreenWindow::screen() const
{
    return _screen;
}

Character* ScreenWindow::getImage()
{
    // reallocate internal buffer if the window size has changed
    int size = windowLines() * windowColumns();
    if (_windowBuffer == 0 || _windowBufferSize != size) {
        delete[] _windowBuffer;
        _windowBufferSize = size;
        _windowBuffer = new Character[size];
        _bufferNeedsUpdate = true;
    }

    if (!_bufferNeedsUpdate)
        return _windowBuffer;

    _screen->getImage(_windowBuffer, size,
                      currentLine(), endWindowLine());

    // this window may look beyond the end of the screen, in which
    // case there will be an unused area which needs to be filled
    // with blank characters
    fillUnusedArea();

    _bufferNeedsUpdate = false;
    return _windowBuffer;
}

void ScreenWindow::fillUnusedArea()
{
    int screenEndLine = _screen->getHistLines() + _screen->getLines() - 1;
    int windowEndLine = currentLine() + windowLines() - 1;

    int unusedLines = windowEndLine - screenEndLine;

    // stop when unusedLines is negative; there is an issue w/ charsToFill
    //  being greater than an int can hold
    if (unusedLines <= 0)
        return;

    int charsToFill = unusedLines * windowColumns();

    Screen::fillWithDefaultChar(_windowBuffer + _windowBufferSize - charsToFill, charsToFill);
}

// return the index of the line at the end of this window, or if this window
// goes beyond the end of the screen, the index of the line at the end
// of the screen.
//
// when passing a line number to a Screen method, the line number should
// never be more than endWindowLine()
//
int ScreenWindow::endWindowLine() const
{
    return std::min(currentLine() + windowLines() - 1,
                lineCount() - 1);
}
QVector<LineProperty> ScreenWindow::getLineProperties()
{
    QVector<LineProperty> result = _screen->getLineProperties(currentLine(), endWindowLine());

    if (result.count() != windowLines())
        result.resize(windowLines());

    return result;
}

QString ScreenWindow::selectedText(bool preserveLineBreaks, bool trimTrailingSpaces) const
{
    return _screen->selectedText(preserveLineBreaks, trimTrailingSpaces);
}

void ScreenWindow::getSelectionStart(int& column , int& line)
{
    _screen->getSelectionStart(column, line);
    line -= currentLine();
}
void ScreenWindow::getSelectionEnd(int& column , int& line)
{
    _screen->getSelectionEnd(column, line);
    line -= currentLine();
}
void ScreenWindow::setSelectionStart(int column , int line , bool columnMode)
{
    _screen->setSelectionStart(column , line + currentLine() , columnMode);

    _bufferNeedsUpdate = true;
    emit selectionChanged();
}

void ScreenWindow::setSelectionEnd(int column , int line)
{
    _screen->setSelectionEnd(column , line + currentLine());

    _bufferNeedsUpdate = true;
    emit selectionChanged();
}

void ScreenWindow::setSelectionByLineRange(int start, int end)
{
    clearSelection();

    _screen->setSelectionStart(0 , start , false);
    _screen->setSelectionEnd(windowColumns() , end);

    _bufferNeedsUpdate = true;
    emit selectionChanged();
}

bool ScreenWindow::isSelected(int column , int line)
{
    return _screen->isSelected(column , std::min(line + currentLine(), endWindowLine()));
}

void ScreenWindow::clearSelection()
{
    _screen->clearSelection();

    emit selectionChanged();
}

void ScreenWindow::setWindowLines(int lines)
{
    Q_ASSERT(lines > 0);
    _windowLines = lines;
}
int ScreenWindow::windowLines() const
{
    return _windowLines;
}

int ScreenWindow::windowColumns() const
{
    return _screen->getColumns();
}

int ScreenWindow::lineCount() const
{
    return _screen->getHistLines() + _screen->getLines();
}

int ScreenWindow::columnCount() const
{
    return _screen->getColumns();
}

QPoint ScreenWindow::cursorPosition() const
{
    QPoint position;

    position.setX(_screen->getCursorX());
    position.setY(_screen->getCursorY());

    return position;
}

int ScreenWindow::currentLine() const
{
    return std::clamp(_currentLine, 0, std::max(0, lineCount() - windowLines()));
}

int ScreenWindow::currentResultLine() const
{
    return _currentResultLine;
}

void ScreenWindow::setCurrentResultLine(int line)
{
    if (_currentResultLine == line) {
        return;
    }

    _currentResultLine = line;
    emit currentResultLineChanged();
}

void ScreenWindow::scrollBy(RelativeScrollMode mode, int amount, bool fullPage)
{
    if (mode == ScrollLines) {
        scrollTo(currentLine() + amount);
    } else if (mode == ScrollPages) {
        if (fullPage)
            scrollTo(currentLine() + amount * (windowLines()));
        else
            scrollTo(currentLine() + amount * (windowLines() / 2));
    }
}

bool ScreenWindow::atEndOfOutput() const
{
    return currentLine() == (lineCount() - windowLines());
}

void ScreenWindow::scrollTo(int line)
{
    int maxCurrentLineNumber = lineCount() - windowLines();
    line = std::clamp(line, 0, maxCurrentLineNumber);

    const int delta = line - _currentLine;
    _currentLine = line;

    // keep track of number of lines scrolled by,
    // this can be reset by calling resetScrollCount()
    _scrollCount += delta;

    _bufferNeedsUpdate = true;

    emit scrolled(_currentLine);
}

void ScreenWindow::setTrackOutput(bool trackOutput)
{
    _trackOutput = trackOutput;
}

bool ScreenWindow::trackOutput() const
{
    return _trackOutput;
}

int ScreenWindow::scrollCount() const
{
    return _scrollCount;
}

void ScreenWindow::resetScrollCount()
{
    _scrollCount = 0;
}

QRect ScreenWindow::scrollRegion() const
{
    bool equalToScreenSize = windowLines() == _screen->getLines();

    if (atEndOfOutput() && equalToScreenSize)
        return _screen->lastScrolledRegion();
    else
        return QRect(0, 0, windowColumns(), windowLines());
}

void ScreenWindow::notifyOutputChanged()
{
    // move window to the bottom of the screen and update scroll count
    // if this window is currently tracking the bottom of the screen
    if (_trackOutput) {
        _scrollCount -= _screen->scrolledLines();
        _currentLine = std::max(0, _screen->getHistLines() - (windowLines() - _screen->getLines()));
    } else {
        // if the history is not unlimited then it may
        // have run out of space and dropped the oldest
        // lines of output - in this case the screen
        // window's current line number will need to
        // be adjusted - otherwise the output will scroll
        _currentLine = std::max(0, _currentLine -
                            _screen->droppedLines());

        // ensure that the screen window's current position does
        // not go beyond the bottom of the screen
        _currentLine = std::min(_currentLine , _screen->getHistLines());
    }

    _bufferNeedsUpdate = true;

    emit outputChanged();
}

#include "ScreenWindow.moc"
