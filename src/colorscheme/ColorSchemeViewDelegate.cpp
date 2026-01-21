/*
    Copyright 2007-2008 by Robert Knight <robertknight@gmail.com>
    Copyright 2018 by Harald Sitter <sitter@kde.org>

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
#include "ColorSchemeViewDelegate.h"

// Konsole
#include "ColorScheme.h"

// KDE
#include <KWindowSystem>
#include <KLocalizedString>

// Qt
#include <QPainter>
#include <QApplication>

using namespace Konsole;

ColorSchemeViewDelegate::ColorSchemeViewDelegate(QObject* parent)
    : QAbstractItemDelegate(parent)
{
}

void ColorSchemeViewDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const
{
    const ColorScheme* scheme = index.data(Qt::UserRole + 1).value<const ColorScheme*>();
    Q_ASSERT(scheme);
    if (!scheme)
        return;

    bool transparencyAvailable = KWindowSystem::compositingActive();

    painter->setRenderHint(QPainter::Antialiasing);

    // draw background
    painter->setPen(QPen(scheme->foregroundColor() , 1));

    // radial gradient for background
    // from a lightened version of the scheme's background color in the center to
    // a darker version at the outer edge
    QColor color = scheme->backgroundColor();
    QRectF backgroundRect = QRectF(option.rect).adjusted(1.5, 1.5, -1.5, -1.5);

    QRadialGradient backgroundGradient(backgroundRect.center() , backgroundRect.width() / 2);
    backgroundGradient.setColorAt(0 , color.lighter(105));
    backgroundGradient.setColorAt(1 , color.darker(115));

    const int backgroundRectXRoundness = 4;
    const int backgroundRectYRoundness = 30;

    QPainterPath backgroundRectPath(backgroundRect.topLeft());
    backgroundRectPath.addRoundRect(backgroundRect , backgroundRectXRoundness , backgroundRectYRoundness);

    if (transparencyAvailable) {
        painter->save();
        color.setAlphaF(scheme->opacity());
        painter->setCompositionMode(QPainter::CompositionMode_Source);
        painter->setBrush(backgroundGradient);

        painter->drawPath(backgroundRectPath);
        painter->restore();
    } else {
        painter->setBrush(backgroundGradient);
        painter->drawPath(backgroundRectPath);
    }

    // draw stripe at the side using scheme's foreground color
    painter->setPen(QPen(Qt::NoPen));
    QPainterPath path(option.rect.topLeft());
    path.lineTo(option.rect.width() / 10.0 , option.rect.top());
    path.lineTo(option.rect.bottomLeft());
    path.lineTo(option.rect.topLeft());
    painter->setBrush(scheme->foregroundColor());
    painter->drawPath(path.intersected(backgroundRectPath));

    // draw highlight
    // with a linear gradient going from translucent white to transparent
    QLinearGradient gradient(option.rect.topLeft() , option.rect.bottomLeft());
    gradient.setColorAt(0 , QColor(255, 255, 255, 90));
    gradient.setColorAt(1 , Qt::transparent);
    painter->setBrush(gradient);
    painter->drawRoundRect(backgroundRect , 4 , 30);

    const bool isSelected = option.state & QStyle::State_Selected;

    // draw border on selected items
    if (isSelected) {
        static const int selectedBorderWidth = 6;

        painter->setBrush(QBrush(Qt::NoBrush));
        QPen pen;

        QColor highlightColor = option.palette.highlight().color();
        highlightColor.setAlphaF(1.0);

        pen.setBrush(highlightColor);
        pen.setWidth(selectedBorderWidth);
        pen.setJoinStyle(Qt::MiterJoin);

        painter->setPen(pen);

        painter->drawRect(option.rect.adjusted(selectedBorderWidth / 2,
                                               selectedBorderWidth / 2,
                                               -selectedBorderWidth / 2,
                                               -selectedBorderWidth / 2));
    }

    // draw color scheme name using scheme's foreground color
    QPen pen(scheme->foregroundColor());
    painter->setPen(pen);

    painter->drawText(option.rect , Qt::AlignCenter ,
                      index.data(Qt::DisplayRole).toString());
}

QSize ColorSchemeViewDelegate::sizeHint(const QStyleOptionViewItem& option,
                                        const QModelIndex& /*index*/) const
{
    const int width = 200;
    const int margin = 5;
    const int colorWidth = width / TABLE_COLORS;
    const int heightForWidth = (colorWidth * 2) + option.fontMetrics.height() + margin;

    // temporary
    return QSize(width, heightForWidth);
}

