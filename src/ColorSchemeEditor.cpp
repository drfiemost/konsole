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
#include "ColorSchemeEditor.h"

// Qt
#include <QFontMetrics>
#include <QFileInfo>

// KDE
#include <KColorDialog>
#include <KWindowSystem>
#include <KFileDialog>
#include <KUrlCompletion>

// Konsole
#include "ui_ColorSchemeEditor.h"
#include "ColorScheme.h"
#include "CharacterColor.h"

using namespace Konsole;

// colorTable is half the length of _table in ColorScheme class
// since intense colors are in a separated column
const int COLOR_TABLE_ROW_LENGTH =  TABLE_COLORS / 3;

const int NAME_COLUMN = 0;           // column 0 : color names
const int COLOR_COLUMN = 1;          // column 1 : actual colors
const int INTENSE_COLOR_COLUMN = 2;  // column 2 : intense colors
const int FAINT_COLOR_COLUMN = 3;    // column 2 : faint colors

ColorSchemeEditor::ColorSchemeEditor(QWidget* aParent)
    : KDialog(aParent)
    , _isNewScheme(false)
    , _ui(nullptr)
    , _colors(nullptr)
{
    // Kdialog buttons
    setButtons(KDialog::Ok | KDialog::Cancel | KDialog::Apply);
    connect(this, &Konsole::ColorSchemeEditor::applyClicked, this, &Konsole::ColorSchemeEditor::saveColorScheme);
    connect(this, &Konsole::ColorSchemeEditor::okClicked, this, &Konsole::ColorSchemeEditor::saveColorScheme);

    // ui
    _ui = new Ui::ColorSchemeEditor();
    _ui->setupUi(mainWidget());

    // description edit
    _ui->descriptionEdit->setClearButtonShown(true);
    connect(_ui->descriptionEdit , &QLineEdit::textChanged , this ,
            &Konsole::ColorSchemeEditor::setDescription);

    // transparency slider
    QFontMetrics metrics(font());
    _ui->transparencyPercentLabel->setMinimumWidth(metrics.width(QStringLiteral("100%")));

    connect(_ui->transparencySlider , &QSlider::valueChanged , this , &Konsole::ColorSchemeEditor::setTransparencyPercentLabel);

    // randomized background
    connect(_ui->randomizedBackgroundCheck , &QCheckBox::toggled , this ,
            &Konsole::ColorSchemeEditor::setRandomizedBackgroundColor);

    // wallpaper stuff
    KUrlCompletion* fileCompletion = new KUrlCompletion(KUrlCompletion::FileCompletion);
    fileCompletion->setParent(this);
    _ui->wallpaperPath->setCompletionObject(fileCompletion);
    _ui->wallpaperPath->setClearButtonShown(true);
    _ui->wallpaperSelectButton->setIcon(KIcon(QStringLiteral("image-x-generic")));

    connect(_ui->wallpaperSelectButton, &QToolButton::clicked,
            this, &Konsole::ColorSchemeEditor::selectWallpaper);
    connect(_ui->wallpaperPath, &QLineEdit::textChanged,
            this, &Konsole::ColorSchemeEditor::wallpaperPathChanged);

    // color table
    _ui->colorTable->setColumnCount(4);
    _ui->colorTable->setRowCount(COLOR_TABLE_ROW_LENGTH);

    QStringList labels;
    labels << i18nc("@label:listbox Column header text for color names", "Name")
           << i18nc("@label:listbox Column header text for the actual colors", "Color")
           << i18nc("@label:listbox Column header text for the actual intense colors", "Intense color")
           << i18nc("@label:listbox Column header text for the actual faint colors", "Faint color");
    _ui->colorTable->setHorizontalHeaderLabels(labels);

    // Set resize mode for colorTable columns
    _ui->colorTable->horizontalHeader()->setResizeMode(NAME_COLUMN, QHeaderView::ResizeToContents);
    _ui->colorTable->horizontalHeader()->setResizeMode(COLOR_COLUMN, QHeaderView::Stretch);
    _ui->colorTable->horizontalHeader()->setResizeMode(INTENSE_COLOR_COLUMN, QHeaderView::Stretch);
    _ui->colorTable->horizontalHeader()->setResizeMode(FAINT_COLOR_COLUMN, QHeaderView::Stretch);

    QTableWidgetItem* item = new QTableWidgetItem(QStringLiteral("Test"));
    _ui->colorTable->setItem(0, 0, item);

    _ui->colorTable->verticalHeader()->hide();

    connect(_ui->colorTable , &QTableWidget::itemClicked , this ,
            &Konsole::ColorSchemeEditor::editColorItem);

    // warning label when transparency is not available
    _ui->transparencyWarningWidget->setWordWrap(true);
    _ui->transparencyWarningWidget->setCloseButtonVisible(false);
    _ui->transparencyWarningWidget->setMessageType(KMessageWidget::Warning);

    if (KWindowSystem::compositingActive()) {
        _ui->transparencyWarningWidget->setVisible(false);
    } else {
        _ui->transparencyWarningWidget->setText(i18nc("@info:status",
                                                "The background transparency setting will not"
                                                " be used because your desktop does not appear to support"
                                                " transparent windows."));
    }
}
ColorSchemeEditor::~ColorSchemeEditor()
{
    delete _colors;
    delete _ui;
}
void ColorSchemeEditor::editColorItem(QTableWidgetItem* item)
{
    // ignore if this is not a color column
    if (item->column() != COLOR_COLUMN && item->column() != INTENSE_COLOR_COLUMN && item->column() != FAINT_COLOR_COLUMN) {
        return;
    }

    QColor color = item->background().color();
    int result = KColorDialog::getColor(color);

    if (result == KColorDialog::Accepted) {
        item->setBackground(color);

        int colorSchemeRow = item->row();
        // Intense colors row are in the middle third of the color table
        if (item->column() == INTENSE_COLOR_COLUMN) {
            colorSchemeRow += COLOR_TABLE_ROW_LENGTH;
        }

        // and the faint color rows are in the middle third of the color table
        if (item->column() == FAINT_COLOR_COLUMN) {
            colorSchemeRow += 2*COLOR_TABLE_ROW_LENGTH;
        }

        ColorEntry entry(_colors->colorEntry(colorSchemeRow));
        entry = color;
        _colors->setColorTableEntry(colorSchemeRow, entry);

        emit colorsChanged(_colors);
    }
}
void ColorSchemeEditor::selectWallpaper()
{
    const KUrl url = KFileDialog::getImageOpenUrl(_ui->wallpaperPath->text(),
                     this,
                     i18nc("@action:button", "Select wallpaper image file"));

    if (!url.isEmpty())
        _ui->wallpaperPath->setText(url.path());
}
void ColorSchemeEditor::wallpaperPathChanged(const QString& path)
{
    if (path.isEmpty()) {
        _colors->setWallpaper(path);
    } else {
        QFileInfo i(path);

        if (i.exists() && i.isFile() && i.isReadable())
            _colors->setWallpaper(path);
    }
}
void ColorSchemeEditor::setDescription(const QString& description)
{
    if (_colors)
        _colors->setDescription(description);

    if (_ui->descriptionEdit->text() != description)
        _ui->descriptionEdit->setText(description);
}
void ColorSchemeEditor::setTransparencyPercentLabel(int percent)
{
    _ui->transparencyPercentLabel->setText(QStringLiteral("%1%").arg(percent));

    const qreal opacity = (100.0 - percent) / 100.0;
    _colors->setOpacity(opacity);
}
void ColorSchemeEditor::setRandomizedBackgroundColor(bool randomized)
{
    _colors->setRandomizedBackgroundColor(randomized);
}
void ColorSchemeEditor::setup(const ColorScheme* scheme, bool isNewScheme)
{
    _isNewScheme = isNewScheme;

    delete _colors;

    _colors = new ColorScheme(*scheme);

    if (_isNewScheme) {
        setCaption(i18nc("@title:window", "New Color Scheme"));
        setDescription(i18nc("@title:window", "Edit Color Scheme"));
    } else {
        setCaption(i18n("Edit Color Scheme"));
    }

    // setup description edit
    _ui->descriptionEdit->setText(_colors->description());

    // setup color table
    setupColorTable(_colors);

    // setup transparency slider
    const int transparencyPercent = qRound((1 - _colors->opacity()) * 100);
    _ui->transparencySlider->setValue(transparencyPercent);
    setTransparencyPercentLabel(transparencyPercent);

    // randomized background color checkbox
    _ui->randomizedBackgroundCheck->setChecked(scheme->randomizedBackgroundColor());

    // wallpaper stuff
    _ui->wallpaperPath->setText(scheme->wallpaper()->path());
}
void ColorSchemeEditor::setupColorTable(const ColorScheme* colors)
{
    ColorEntry table[TABLE_COLORS];
    colors->getColorTable(table);

    for (int row = 0; row < COLOR_TABLE_ROW_LENGTH; row++) {
        QTableWidgetItem* nameItem = new QTableWidgetItem(ColorScheme::translatedColorNameForIndex(row));
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);

        QTableWidgetItem* colorItem = new QTableWidgetItem();
        colorItem->setBackground(table[row]);
        colorItem->setFlags(colorItem->flags() & ~Qt::ItemIsEditable & ~Qt::ItemIsSelectable);
        colorItem->setToolTip(i18nc("@info:tooltip", "Click to choose color"));

        QTableWidgetItem* colorItemIntense = new QTableWidgetItem();
        colorItemIntense->setBackground(table[COLOR_TABLE_ROW_LENGTH + row]);
        colorItemIntense->setFlags(colorItem->flags() & ~Qt::ItemIsEditable & ~Qt::ItemIsSelectable);
        colorItemIntense->setToolTip(i18nc("@info:tooltip", "Click to choose intense color"));

        QTableWidgetItem* colorItemFaint = new QTableWidgetItem();
        colorItemFaint->setBackground(table[2*COLOR_TABLE_ROW_LENGTH + row]);
        colorItemFaint->setFlags(colorItem->flags() & ~Qt::ItemIsEditable & ~Qt::ItemIsSelectable);
        colorItemFaint->setToolTip(i18nc("@info:tooltip", "Click to choose Faint color"));

        _ui->colorTable->setItem(row, NAME_COLUMN, nameItem);
        _ui->colorTable->setItem(row, COLOR_COLUMN, colorItem);
        _ui->colorTable->setItem(row, INTENSE_COLOR_COLUMN, colorItemIntense);
        _ui->colorTable->setItem(row, FAINT_COLOR_COLUMN, colorItemFaint);
    }
    // ensure that color names are as fully visible as possible
    _ui->colorTable->resizeColumnToContents(0);

    // set the widget height to the table content
    _ui->colorTable->setFixedHeight(_ui->colorTable->verticalHeader()->length() + _ui->colorTable->horizontalHeader()->height() + 2);
}

ColorScheme& ColorSchemeEditor::colorScheme() const
{
    return *_colors;
}
bool ColorSchemeEditor::isNewScheme() const
{
    return _isNewScheme;
}
void ColorSchemeEditor::saveColorScheme()
{
    emit colorSchemeSaveRequested(colorScheme(), _isNewScheme);
}

#include "ColorSchemeEditor.moc"
