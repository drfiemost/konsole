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
#include "EditProfileDialog.h"

// Standard
#include <cmath>

// Qt
#include <QBrush>
#include <QPainter>
#include <QStandardItem>
#include <QTextCodec>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QTimer>

// KDE
#include <KCodecAction>
#include <KFontDialog>
#include <KIcon>
#include <KIconDialog>
#include <KFileDialog>
#include <KUrlCompletion>
#include <KWindowSystem>
#include <KTextEdit>
#include <KMessageBox>

// Konsole
#include "ColorSchemeManager.h"
#include "ui_EditProfileDialog.h"
#include "KeyBindingEditor.h"
#include "KeyboardTranslator.h"
#include "KeyboardTranslatorManager.h"
#include "ProfileManager.h"
#include "ShellCommand.h"
#include "WindowSystemInfo.h"

using namespace Konsole;

EditProfileDialog::EditProfileDialog(QWidget* parent)
    : KDialog(parent)
    , _ui(nullptr)
    , _tempProfile(nullptr)
    , _profile(nullptr)
    , _pageNeedsUpdate(QVector<bool>())
    , _previewedProperties(QHash<int, QVariant>())
    , _delayedPreviewProperties(QHash<int, QVariant>())
    , _delayedPreviewTimer(new QTimer(this))
    , _colorDialog(nullptr)
{
    setCaption(i18n("Edit Profile"));
    setButtons(KDialog::Ok | KDialog::Cancel | KDialog::Apply);

    // disable the apply button , since no modification has been made
    enableButtonApply(false);

    connect(this, &Konsole::EditProfileDialog::applyClicked, this, &Konsole::EditProfileDialog::apply);

    connect(_delayedPreviewTimer, &QTimer::timeout, this, &Konsole::EditProfileDialog::delayedPreviewActivate);

    _ui = new Ui::EditProfileDialog();
    _ui->setupUi(mainWidget());

    // there are various setupXYZPage() methods to load the items
    // for each page and update their states to match the profile
    // being edited.
    //
    // these are only called when needed ( ie. when the user clicks
    // the tab to move to that page ).
    //
    // the _pageNeedsUpdate vector keeps track of the pages that have
    // not been updated since the last profile change and will need
    // to be refreshed when the user switches to them
    _pageNeedsUpdate.resize(_ui->tabWidget->count());
    connect(_ui->tabWidget, &QTabWidget::currentChanged, this,
            &Konsole::EditProfileDialog::preparePage);

    createTempProfile();
}
EditProfileDialog::~EditProfileDialog()
{
    delete _ui;
}
void EditProfileDialog::save()
{
    if (_tempProfile->isEmpty())
        return;

    ProfileManager::instance()->changeProfile(_profile, _tempProfile->setProperties());

    // ensure that these settings are not undone by a call
    // to unpreview()
    QHashIterator<Profile::Property, QVariant> iter(_tempProfile->setProperties());
    while (iter.hasNext()) {
        iter.next();
        _previewedProperties.remove(iter.key());
    }

    createTempProfile();

    enableButtonApply(false);
}
void EditProfileDialog::reject()
{
    unpreviewAll();
    KDialog::reject();
}
void EditProfileDialog::accept()
{
    if (isButtonEnabled(KDialog::Apply)) {
        if (!isValidProfileName()) {
            return;
        }
        save();
    }
    unpreviewAll();
    QDialog::accept();
}

void EditProfileDialog::apply()
{
    if (!isValidProfileName()) {
        return;
    }
    save();
}

bool EditProfileDialog::isValidProfileName()
{
    Q_ASSERT(_profile);
    Q_ASSERT(_tempProfile);

    // check whether the user has enough permissions to save the profile
    QFileInfo fileInfo(_profile->path());
    if (fileInfo.exists() && !fileInfo.isWritable()) {
        if (!_tempProfile->isPropertySet(Profile::Name)
            || (_tempProfile->name() == _profile->name())) {
                KMessageBox::sorry(this,
                                   i18n("<p>Konsole does not have permission to save this profile to:<br /> \"%1\"</p>"
                                        "<p>To be able to save settings you can either change the permissions "
                                        "of the profile configuration file or change the profile name to save "
                                        "the settings to a new profile.</p>", _profile->path()));
                return false;
        }
    }

    const QList<Profile::Ptr> existingProfiles = ProfileManager::instance()->allProfiles();
    QStringList otherExistingProfileNames;

    for(auto existingProfile: existingProfiles) {
        if (existingProfile->name() != _profile->name()) {
            otherExistingProfileNames.append(existingProfile->name());
        }
    }

    if ((_tempProfile->isPropertySet(Profile::Name) &&
            _tempProfile->name().isEmpty())
            || (_profile->name().isEmpty() && _tempProfile->name().isEmpty())) {
        KMessageBox::sorry(this,
                           i18n("<p>Each profile must have a name before it can be saved "
                                "into disk.</p>"));
        // revert the name in the dialog
        _ui->profileNameEdit->setText(_profile->name());
        selectProfileName();
        return false;
    } else if (!_tempProfile->name().isEmpty() && otherExistingProfileNames.contains(_tempProfile->name())) {
        KMessageBox::sorry(this,
                            i18n("<p>A profile with this name already exists.</p>"));
        // revert the name in the dialog
        _ui->profileNameEdit->setText(_profile->name());
        selectProfileName();
        return false;
    } else {
        return true;
    }
}

QString EditProfileDialog::groupProfileNames(const ProfileGroup::Ptr group, int maxLength)
{
    QString caption;
    int count = group->profiles().count();
    for (int i = 0; i < count; i++) {
        caption += group->profiles()[i]->name();
        if (i < (count - 1)) {
            caption += QLatin1Char(',');
            // limit caption length to prevent very long window titles
            if (maxLength > 0 && caption.length() > maxLength) {
                caption += QLatin1String("...");
                break;
            }
        }
    }
    return caption;
}
void EditProfileDialog::updateCaption(const Profile::Ptr profile)
{
    const int MAX_GROUP_CAPTION_LENGTH = 25;
    ProfileGroup::Ptr group = profile->asGroup();
    if (group && group->profiles().count() > 1) {
        QString caption = groupProfileNames(group, MAX_GROUP_CAPTION_LENGTH);
        setCaption(i18np("Editing profile: %2",
                         "Editing %1 profiles: %2",
                         group->profiles().count(),
                         caption));
    } else {
        setCaption(i18n("Edit Profile \"%1\"", profile->name()));
    }
}
void EditProfileDialog::setProfile(Profile::Ptr profile)
{
    Q_ASSERT(profile);

    _profile = profile;

    // update caption
    updateCaption(profile);

    // mark each page of the dialog as out of date
    // and force an update of the currently visible page
    //
    // the other pages will be updated as necessary
    _pageNeedsUpdate.fill(true);
    preparePage(_ui->tabWidget->currentIndex());

    if (_tempProfile) {
        createTempProfile();
    }
}
const Profile::Ptr EditProfileDialog::lookupProfile() const
{
    return _profile;
}

const QString EditProfileDialog::currentColorSchemeName() const
{
    const QString &currentColorSchemeName = lookupProfile()->colorScheme();
    return currentColorSchemeName;
}

void EditProfileDialog::preparePage(int page)
{
    const Profile::Ptr profile = lookupProfile();

    Q_ASSERT(_pageNeedsUpdate.count() > page);
    Q_ASSERT(profile);

    QWidget* pageWidget = _ui->tabWidget->widget(page);

    if (_pageNeedsUpdate[page]) {
        if (pageWidget == _ui->generalTab)
            setupGeneralPage(profile);
        else if (pageWidget == _ui->tabsTab)
            setupTabsPage(profile);
        else if (pageWidget == _ui->appearanceTab)
            setupAppearancePage(profile);
        else if (pageWidget == _ui->scrollingTab)
            setupScrollingPage(profile);
        else if (pageWidget == _ui->keyboardTab)
            setupKeyboardPage(profile);
        else if (pageWidget == _ui->mouseTab)
            setupMousePage(profile);
        else if (pageWidget == _ui->advancedTab)
            setupAdvancedPage(profile);
        else
            Q_ASSERT(false);

        _pageNeedsUpdate[page] = false;
    }
}
void EditProfileDialog::selectProfileName()
{
    _ui->profileNameEdit->setFocus();
    _ui->profileNameEdit->selectAll();
}
void EditProfileDialog::setupGeneralPage(const Profile::Ptr profile)
{
    // basic profile options
    {
        _ui->emptyNameWarningWidget->setWordWrap(false);
        _ui->emptyNameWarningWidget->setCloseButtonVisible(false);
        _ui->emptyNameWarningWidget->setMessageType(KMessageWidget::Warning);

        ProfileGroup::Ptr group = profile->asGroup();
        if (!group || group->profiles().count() < 2) {
            _ui->profileNameEdit->setText(profile->name());
            _ui->profileNameEdit->setClearButtonShown(true);

            _ui->emptyNameWarningWidget->setVisible(profile->name().isEmpty());
            _ui->emptyNameWarningWidget->setText(i18n("Profile name is empty."));
        } else {
            _ui->profileNameEdit->setText(groupProfileNames(group, -1));
            _ui->profileNameEdit->setEnabled(false);
            _ui->profileNameLabel->setEnabled(false);

            _ui->emptyNameWarningWidget->setVisible(false);
        }
    }

    ShellCommand command(profile->command() , profile->arguments());
    _ui->commandEdit->setText(command.fullCommand());
    KUrlCompletion* exeCompletion = new KUrlCompletion(KUrlCompletion::ExeCompletion);
    exeCompletion->setParent(this);
    exeCompletion->setDir(QString());
    _ui->commandEdit->setCompletionObject(exeCompletion);

    _ui->initialDirEdit->setText(profile->defaultWorkingDirectory());
    KUrlCompletion* dirCompletion = new KUrlCompletion(KUrlCompletion::DirCompletion);
    dirCompletion->setParent(this);
    _ui->initialDirEdit->setCompletionObject(dirCompletion);
    _ui->initialDirEdit->setText(profile->defaultWorkingDirectory());
    _ui->initialDirEdit->setClearButtonShown(true);

    _ui->dirSelectButton->setIcon(KIcon(QStringLiteral("folder-open")));
    _ui->iconSelectButton->setIcon(KIcon(profile->icon()));
    _ui->startInSameDirButton->setChecked(profile->startInCurrentSessionDir());

    // terminal options
    _ui->terminalColumnsEntry->setValue(profile->terminalColumns());
    _ui->terminalRowsEntry->setValue(profile->terminalRows());

    // window options
    _ui->showTerminalSizeHintButton->setChecked(profile->showTerminalSizeHint());

    // signals and slots
    connect(_ui->dirSelectButton, &QToolButton::clicked, this, &Konsole::EditProfileDialog::selectInitialDir);
    connect(_ui->iconSelectButton, &QPushButton::clicked, this, &Konsole::EditProfileDialog::selectIcon);
    connect(_ui->startInSameDirButton, &QCheckBox::toggled, this ,
            &Konsole::EditProfileDialog::startInSameDir);
    connect(_ui->profileNameEdit, &QLineEdit::textChanged, this,
            &Konsole::EditProfileDialog::profileNameChanged);
    connect(_ui->initialDirEdit, &QLineEdit::textChanged, this,
            &Konsole::EditProfileDialog::initialDirChanged);
    connect(_ui->commandEdit, &QLineEdit::textChanged, this,
            &Konsole::EditProfileDialog::commandChanged);
    connect(_ui->environmentEditButton , &QPushButton::clicked, this,
            &Konsole::EditProfileDialog::showEnvironmentEditor);

    connect(_ui->terminalColumnsEntry, static_cast<void(KIntSpinBox::*)(int)>(&KIntSpinBox::valueChanged),
            this, &Konsole::EditProfileDialog::terminalColumnsEntryChanged);
    connect(_ui->terminalRowsEntry, static_cast<void(KIntSpinBox::*)(int)>(&KIntSpinBox::valueChanged),
            this, &Konsole::EditProfileDialog::terminalRowsEntryChanged);

    connect(_ui->showTerminalSizeHintButton, &QCheckBox::toggled, this,
            &Konsole::EditProfileDialog::showTerminalSizeHint);
}
void EditProfileDialog::showEnvironmentEditor()
{
    const Profile::Ptr profile = lookupProfile();

    QStringList currentEnvironment;

    // The user could re-open the environment editor before clicking
    // OK/Apply in the parent edit profile dialog, so we make sure
    // to show the new environment vars
    if (_tempProfile->isPropertySet(Profile::Environment)) {
            currentEnvironment  = _tempProfile->environment();
    } else {
        currentEnvironment = profile->environment();
    }

    QWeakPointer<KDialog> dialog = new KDialog(this);
    KTextEdit* edit = new KTextEdit(dialog.data());

    edit->setPlainText(currentEnvironment.join(QStringLiteral("\n")));
    edit->setToolTip(i18nc("@info:tooltip", "One environment variable per line"));

    dialog.data()->setPlainCaption(i18n("Edit Environment"));
    dialog.data()->setMainWidget(edit);

    if (dialog.data()->exec() == QDialog::Accepted) {
        QStringList newEnvironment = edit->toPlainText().split(QLatin1Char('\n'));
        updateTempProfileProperty(Profile::Environment, newEnvironment);
    }

    delete dialog.data();
}
void EditProfileDialog::setupTabsPage(const Profile::Ptr profile)
{
    // tab title format
    _ui->renameTabWidget->setTabTitleText(profile->localTabTitleFormat());
    _ui->renameTabWidget->setRemoteTabTitleText(profile->remoteTabTitleFormat());

    connect(_ui->renameTabWidget, &Konsole::RenameTabWidget::tabTitleFormatChanged, this,
            &Konsole::EditProfileDialog::tabTitleFormatChanged);
    connect(_ui->renameTabWidget, &Konsole::RenameTabWidget::remoteTabTitleFormatChanged, this,
            &Konsole::EditProfileDialog::remoteTabTitleFormatChanged);

    // tab monitoring
    const int silenceSeconds = profile->silenceSeconds();
    _ui->silenceSecondsSpinner->setValue(silenceSeconds);
    _ui->silenceSecondsSpinner->setSuffix(ki18ncp("Unit of time", " second", " seconds"));

    connect(_ui->silenceSecondsSpinner, static_cast<void(KIntSpinBox::*)(int)>(&KIntSpinBox::valueChanged),
            this, &Konsole::EditProfileDialog::silenceSecondsChanged);
}

void EditProfileDialog::terminalColumnsEntryChanged(int value)
{
    updateTempProfileProperty(Profile::TerminalColumns, value);
}
void EditProfileDialog::terminalRowsEntryChanged(int value)
{
    updateTempProfileProperty(Profile::TerminalRows, value);
}
void EditProfileDialog::showTerminalSizeHint(bool value)
{
    updateTempProfileProperty(Profile::ShowTerminalSizeHint, value);
}
void EditProfileDialog::tabTitleFormatChanged(const QString& format)
{
    updateTempProfileProperty(Profile::LocalTabTitleFormat, format);
}
void EditProfileDialog::remoteTabTitleFormatChanged(const QString& format)
{
    updateTempProfileProperty(Profile::RemoteTabTitleFormat, format);
}

void EditProfileDialog::silenceSecondsChanged(int seconds)
{
    updateTempProfileProperty(Profile::SilenceSeconds, seconds);
}

void EditProfileDialog::selectIcon()
{
    const QString& icon = KIconDialog::getIcon(KIconLoader::Desktop, KIconLoader::Application,
                          false, 0, false, this);
    if (!icon.isEmpty()) {
        _ui->iconSelectButton->setIcon(KIcon(icon));
        updateTempProfileProperty(Profile::Icon, icon);
    }
}
void EditProfileDialog::profileNameChanged(const QString& name)
{
    _ui->emptyNameWarningWidget->setVisible(name.isEmpty());

    updateTempProfileProperty(Profile::Name, name);
    updateTempProfileProperty(Profile::UntranslatedName, name);
    updateCaption(_tempProfile);
}
void EditProfileDialog::startInSameDir(bool sameDir)
{
    updateTempProfileProperty(Profile::StartInCurrentSessionDir, sameDir);
}
void EditProfileDialog::initialDirChanged(const QString& dir)
{
    updateTempProfileProperty(Profile::Directory, dir);
}
void EditProfileDialog::commandChanged(const QString& command)
{
    ShellCommand shellCommand(command);

    updateTempProfileProperty(Profile::Command, shellCommand.command());
    updateTempProfileProperty(Profile::Arguments, shellCommand.arguments());
}
void EditProfileDialog::selectInitialDir()
{
    const KUrl url = KFileDialog::getExistingDirectoryUrl(_ui->initialDirEdit->text(),
                     this,
                     i18n("Select Initial Directory"));

    if (!url.isEmpty())
        _ui->initialDirEdit->setText(url.path());
}
void EditProfileDialog::setupAppearancePage(const Profile::Ptr profile)
{
    ColorSchemeViewDelegate* delegate = new ColorSchemeViewDelegate(this);
    _ui->colorSchemeList->setItemDelegate(delegate);

    _ui->transparencyWarningWidget->setVisible(false);
    _ui->transparencyWarningWidget->setWordWrap(true);
    _ui->transparencyWarningWidget->setCloseButtonVisible(false);
    _ui->transparencyWarningWidget->setMessageType(KMessageWidget::Warning);

    _ui->editColorSchemeButton->setEnabled(false);
    _ui->removeColorSchemeButton->setEnabled(false);
    _ui->resetColorSchemeButton->setEnabled(false);

    // setup color list
    // select the colorScheme used in the current profile
    updateColorSchemeList(currentColorSchemeName());

    _ui->colorSchemeList->setMouseTracking(true);
    _ui->colorSchemeList->installEventFilter(this);
    _ui->colorSchemeList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    connect(_ui->colorSchemeList->selectionModel(),
            &QItemSelectionModel::selectionChanged,
            this, &Konsole::EditProfileDialog::colorSchemeSelected);
    connect(_ui->colorSchemeList, &QListView::entered, this,
            &Konsole::EditProfileDialog::previewColorScheme);

    updateColorSchemeButtons();

    connect(_ui->editColorSchemeButton, &QPushButton::clicked, this,
            &Konsole::EditProfileDialog::editColorScheme);
    connect(_ui->removeColorSchemeButton, &QPushButton::clicked, this,
            &Konsole::EditProfileDialog::removeColorScheme);
    connect(_ui->newColorSchemeButton, &QPushButton::clicked, this,
            &Konsole::EditProfileDialog::newColorScheme);

    connect(_ui->resetColorSchemeButton, &QPushButton::clicked, this,
            &Konsole::EditProfileDialog::resetColorScheme);

    // setup font preview
    const bool antialias = profile->antiAliasFonts();

    QFont profileFont = profile->font();
    profileFont.setStyleStrategy(antialias ? QFont::PreferAntialias : QFont::NoAntialias);

    _ui->fontPreviewLabel->installEventFilter(this);
    _ui->fontPreviewLabel->setFont(profileFont);
    setFontInputValue(profileFont);

    connect(_ui->fontSizeInput, &KDoubleNumInput::valueChanged, this,
            &Konsole::EditProfileDialog::setFontSize);
    connect(_ui->selectFontButton, &QPushButton::clicked, this,
            &Konsole::EditProfileDialog::showFontDialog);

    // setup font smoothing
    _ui->antialiasTextButton->setChecked(antialias);
    connect(_ui->antialiasTextButton, &QCheckBox::toggled, this,
            &Konsole::EditProfileDialog::setAntialiasText);

    _ui->boldIntenseButton->setChecked(profile->boldIntense());
    connect(_ui->boldIntenseButton, &QCheckBox::toggled, this,
            &Konsole::EditProfileDialog::setBoldIntense);
    _ui->enableMouseWheelZoomButton->setChecked(profile->mouseWheelZoomEnabled());

    _ui->useFontLineCharactersButton->setChecked(profile->useFontLineCharacters());
    connect(_ui->useFontLineCharactersButton, &QCheckBox::toggled, this,
            &Konsole::EditProfileDialog::useFontLineCharacters);

    connect(_ui->enableMouseWheelZoomButton, &QCheckBox::toggled, this,
            &Konsole::EditProfileDialog::toggleMouseWheelZoom);
}
void EditProfileDialog::setAntialiasText(bool enable)
{
    QFont profileFont = _ui->fontPreviewLabel->font();
    profileFont.setStyleStrategy(enable ? QFont::PreferAntialias : QFont::NoAntialias);

    // update preview to reflect text smoothing state
    fontSelected(profileFont);
    updateTempProfileProperty(Profile::AntiAliasFonts, enable);
}
void EditProfileDialog::setBoldIntense(bool enable)
{
    preview(Profile::BoldIntense, enable);
    updateTempProfileProperty(Profile::BoldIntense, enable);
}
void EditProfileDialog::useFontLineCharacters(bool enable)
{
    preview(Profile::UseFontLineCharacters, enable);
    updateTempProfileProperty(Profile::UseFontLineCharacters, enable);
}
void EditProfileDialog::toggleMouseWheelZoom(bool enable)
{
    updateTempProfileProperty(Profile::MouseWheelZoomEnabled, enable);
}

void EditProfileDialog::toggleAlternateScrolling(bool enable)
{
    updateTempProfileProperty(Profile::AlternateScrolling, enable);
}

void EditProfileDialog::updateColorSchemeList(const QString &selectedColorSchemeName)
{
    if (!_ui->colorSchemeList->model())
        _ui->colorSchemeList->setModel(new QStandardItemModel(this));

    const ColorScheme *selectedColorScheme = ColorSchemeManager::instance()->findColorScheme(selectedColorSchemeName);

    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(_ui->colorSchemeList->model());

    Q_ASSERT(model);

    model->clear();

    QStandardItem* selectedItem = nullptr;

    QList<const ColorScheme*> schemeList = ColorSchemeManager::instance()->allColorSchemes();

    for(const ColorScheme* scheme: schemeList) {
        QStandardItem* item = new QStandardItem(scheme->description());
        item->setData(QVariant::fromValue(scheme), Qt::UserRole + 1);
        item->setFlags(item->flags());

        // if selectedColorSchemeName is not empty then select that scheme
        // after saving the changes in the colorScheme editor
        if (selectedColorScheme == scheme) {
            selectedItem = item;
        }

        model->appendRow(item);
    }

    model->sort(0);

    if (selectedItem != nullptr) {
        _ui->colorSchemeList->updateGeometry();
        _ui->colorSchemeList->selectionModel()->setCurrentIndex(selectedItem->index() ,
                QItemSelectionModel::Select);

        // update transparency warning label
        updateTransparencyWarning();
    }
}
void EditProfileDialog::updateKeyBindingsList(const QString &selectKeyBindingsName)
{
    if (!_ui->keyBindingList->model())
        _ui->keyBindingList->setModel(new QStandardItemModel(this));

    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(_ui->keyBindingList->model());

    Q_ASSERT(model);

    model->clear();

    QStandardItem* selectedItem = nullptr;

    const QStringList &translatorNames = _keyManager->allTranslators();
    for (const QString &translatorName : translatorNames) {
        const KeyboardTranslator *translator = _keyManager->findTranslator(translatorName);
        if (!translator) continue;

        QStandardItem* item = new QStandardItem(translator->description());
        item->setEditable(false);
        item->setData(QVariant::fromValue(translator), Qt::UserRole + 1);
        item->setData(QVariant::fromValue(_keyManager->findTranslatorPath(translatorName)), Qt::ToolTipRole);
        item->setIcon(KIcon(QStringLiteral("preferences-desktop-keyboard")));

        if (selectKeyBindingsName == translatorName)
            selectedItem = item;

        model->appendRow(item);
    }

    model->sort(0);

    if (selectedItem) {
        _ui->keyBindingList->selectionModel()->setCurrentIndex(selectedItem->index() ,
                QItemSelectionModel::Select);
    }
}
bool EditProfileDialog::eventFilter(QObject* watched , QEvent* event)
{
    if (watched == _ui->colorSchemeList && event->type() == QEvent::Leave) {
        if (_tempProfile->isPropertySet(Profile::ColorScheme))
            preview(Profile::ColorScheme, _tempProfile->colorScheme());
        else
            unpreview(Profile::ColorScheme);
    }
    if (watched == _ui->fontPreviewLabel && event->type() == QEvent::FontChange) {
        const QFont& labelFont = _ui->fontPreviewLabel->font();
        _ui->fontPreviewLabel->setText(i18n("%1", labelFont.family()));
    }

    return KDialog::eventFilter(watched, event);
}
void EditProfileDialog::unpreviewAll()
{
    _delayedPreviewTimer->stop();
    _delayedPreviewProperties.clear();

    QHash<Profile::Property, QVariant> map;
    QHashIterator<int, QVariant> iter(_previewedProperties);
    while (iter.hasNext()) {
        iter.next();
        map.insert(static_cast<Profile::Property>(iter.key()), iter.value());
    }

    // undo any preview changes
    if (!map.isEmpty())
        ProfileManager::instance()->changeProfile(_profile, map, false);
}
void EditProfileDialog::unpreview(int property)
{
    _delayedPreviewProperties.remove(property);

    if (!_previewedProperties.contains(property))
        return;

    QHash<Profile::Property, QVariant> map;
    map.insert(static_cast<Profile::Property>(property), _previewedProperties[property]);
    ProfileManager::instance()->changeProfile(_profile, map, false);

    _previewedProperties.remove(property);
}
void EditProfileDialog::delayedPreview(int property , const QVariant& value)
{
    _delayedPreviewProperties.insert(property, value);

    _delayedPreviewTimer->stop();
    _delayedPreviewTimer->start(300);
}
void EditProfileDialog::delayedPreviewActivate()
{
    Q_ASSERT(qobject_cast<QTimer*>(sender()));

    QMutableHashIterator<int, QVariant> iter(_delayedPreviewProperties);
    if (iter.hasNext()) {
        iter.next();
        preview(iter.key(), iter.value());
    }
}
void EditProfileDialog::preview(int property , const QVariant& value)
{
    QHash<Profile::Property, QVariant> map;
    map.insert(static_cast<Profile::Property>(property), value);

    _delayedPreviewProperties.remove(property);

    const Profile::Ptr original = lookupProfile();

    // skip previews for profile groups if the profiles in the group
    // have conflicting original values for the property
    //
    // TODO - Save the original values for each profile and use to unpreview properties
    ProfileGroup::Ptr group = original->asGroup();
    if (group && group->profiles().count() > 1 &&
            original->property<QVariant>(static_cast<Profile::Property>(property)).isNull()) {
        return;
    }

    if (!_previewedProperties.contains(property)) {
        _previewedProperties.insert(property , original->property<QVariant>(static_cast<Profile::Property>(property)));
    }

    // temporary change to color scheme
    ProfileManager::instance()->changeProfile(_profile , map , false);
}
void EditProfileDialog::previewColorScheme(const QModelIndex& index)
{
    const QString& name = index.data(Qt::UserRole + 1).value<const ColorScheme*>()->name();

    delayedPreview(Profile::ColorScheme , name);
}
void EditProfileDialog::removeColorScheme()
{
    QModelIndexList selected = _ui->colorSchemeList->selectionModel()->selectedIndexes();

    if (!selected.isEmpty()) {
        const QString& name = selected.first().data(Qt::UserRole + 1).value<const ColorScheme*>()->name();

        if (ColorSchemeManager::instance()->deleteColorScheme(name))
            _ui->colorSchemeList->model()->removeRow(selected.first().row());
    }
}

void EditProfileDialog::resetColorScheme()
{
    QModelIndexList selected = _ui->colorSchemeList->selectionModel()->selectedIndexes();

    if (!selected.isEmpty()) {
        const QString &name = selected.first().data(Qt::UserRole + 1).value<const ColorScheme *>()->name();

        ColorSchemeManager::instance()->deleteColorScheme(name);

        // select the colorScheme used in the current profile
        updateColorSchemeList(currentColorSchemeName());
    }
}

void EditProfileDialog::showColorSchemeEditor(bool isNewScheme)
{
    // Finding selected ColorScheme
    QModelIndexList selected = _ui->colorSchemeList->selectionModel()->selectedIndexes();
    QAbstractItemModel* model = _ui->colorSchemeList->model();
    const ColorScheme* colors = nullptr;
    if (!selected.isEmpty())
        colors = model->data(selected.first(), Qt::UserRole + 1).value<const ColorScheme*>();
    else
        colors = ColorSchemeManager::instance()->defaultColorScheme();

    Q_ASSERT(colors);

    // Setting up ColorSchemeEditor ui
    // close any running ColorSchemeEditor
    if (_colorDialog) {
        closeColorSchemeEditor();
    }
    _colorDialog = new ColorSchemeEditor(this);

    connect(_colorDialog, &Konsole::ColorSchemeEditor::colorSchemeSaveRequested,
            this, &Konsole::EditProfileDialog::saveColorScheme);
    _colorDialog->setup(colors, isNewScheme);

    _colorDialog->show();
}
void EditProfileDialog::closeColorSchemeEditor()
{
    if (_colorDialog) {
        _colorDialog->close();
        delete _colorDialog;
    }
}
void EditProfileDialog::newColorScheme()
{
    showColorSchemeEditor(true);
}
void EditProfileDialog::editColorScheme()
{
    showColorSchemeEditor(false);
}
void EditProfileDialog::saveColorScheme(const ColorScheme& scheme, bool isNewScheme)
{
    ColorScheme* newScheme = new ColorScheme(scheme);

    // if this is a new color scheme, pick a name based on the description
    if (isNewScheme) {
        newScheme->setName(newScheme->description());
    }

    ColorSchemeManager::instance()->addColorScheme(newScheme);

    const QString &selectedColorSchemeName = newScheme->name();

    // select the edited or the new colorScheme after saving the changes
    updateColorSchemeList(selectedColorSchemeName);

    preview(Profile::ColorScheme, newScheme->name());
}
void EditProfileDialog::colorSchemeSelected()
{
    QModelIndexList selected = _ui->colorSchemeList->selectionModel()->selectedIndexes();

    if (!selected.isEmpty()) {
        QAbstractItemModel* model = _ui->colorSchemeList->model();
        const ColorScheme* colors = model->data(selected.first(), Qt::UserRole + 1).value<const ColorScheme*>();
        if (colors) {
            updateTempProfileProperty(Profile::ColorScheme, colors->name());
            previewColorScheme(selected.first());

            updateTransparencyWarning();
        }
    }

    updateColorSchemeButtons();
}
void EditProfileDialog::updateColorSchemeButtons()
{
    enableIfNonEmptySelection(_ui->editColorSchemeButton, _ui->colorSchemeList->selectionModel());

    QModelIndexList selected = _ui->colorSchemeList->selectionModel()->selectedIndexes();

    if (!selected.isEmpty()) {
        const QString &name = selected.first().data(Qt::UserRole + 1).value<const ColorScheme *>()->name();

        bool isResettable = ColorSchemeManager::instance()->canResetColorScheme(name);
        _ui->resetColorSchemeButton->setEnabled(isResettable);

        bool isDeletable = ColorSchemeManager::instance()->isColorSchemeDeletable(name);
        // if a colorScheme can be restored then it can't be deleted
        _ui->removeColorSchemeButton->setEnabled(isDeletable && !isResettable);
    } else {
        _ui->removeColorSchemeButton->setEnabled(false);
        _ui->resetColorSchemeButton->setEnabled(false);
    }
}
void EditProfileDialog::updateKeyBindingsButtons()
{
    QModelIndexList selected = _ui->keyBindingList->selectionModel()->selectedIndexes();

    if (!selected.isEmpty()) {
        _ui->editKeyBindingsButton->setEnabled(true);

        const QString &name = selected.first().data(Qt::UserRole + 1).value<const KeyboardTranslator *>()->name();

        bool isResettable = _keyManager->isTranslatorResettable(name);
        _ui->resetKeyBindingsButton->setEnabled(isResettable);

        bool isDeletable = _keyManager->isTranslatorDeletable(name);

        // if a key bindings scheme can be reset then it can't be deleted
        _ui->removeKeyBindingsButton->setEnabled(isDeletable && !isResettable);
    }
}
void EditProfileDialog::enableIfNonEmptySelection(QWidget* widget, QItemSelectionModel* selectionModel)
{
    widget->setEnabled(selectionModel->hasSelection());
}
void EditProfileDialog::updateTransparencyWarning()
{
    // zero or one indexes can be selected
    for(const QModelIndex & index: _ui->colorSchemeList->selectionModel()->selectedIndexes()) {
        bool needTransparency = index.data(Qt::UserRole + 1).value<const ColorScheme*>()->opacity() < 1.0;

        if (!needTransparency) {
            _ui->transparencyWarningWidget->setHidden(true);
        } else if (!KWindowSystem::compositingActive()) {
            _ui->transparencyWarningWidget->setText(i18n("This color scheme uses a transparent background"
                                                    " which does not appear to be supported on your"
                                                    " desktop"));
            _ui->transparencyWarningWidget->setHidden(false);
        } else if (!WindowSystemInfo::HAVE_TRANSPARENCY) {
            _ui->transparencyWarningWidget->setText(i18n("Konsole was started before desktop effects were enabled."
                                                    " You need to restart Konsole to see transparent background."));
            _ui->transparencyWarningWidget->setHidden(false);
        }
    }
}

void EditProfileDialog::createTempProfile()
{
    _tempProfile = Profile::Ptr(new Profile);
    _tempProfile->setHidden(true);
}

void EditProfileDialog::updateTempProfileProperty(Profile::Property property, const QVariant & value)
{
    _tempProfile->setProperty(property, value);
    updateButtonApply();
}

void EditProfileDialog::updateButtonApply()
{
    bool userModified = false;

    QHashIterator<Profile::Property, QVariant> iter(_tempProfile->setProperties());
    while (iter.hasNext()) {
        iter.next();

        Profile::Property property = iter.key();
        QVariant value = iter.value();

        // for previewed property
        if (_previewedProperties.contains(static_cast<int>(property))) {
            if (value != _previewedProperties.value(static_cast<int>(property))) {
                userModified = true;
                break;
            }
        // for not-previewed property
        //
        // for the Profile::KeyBindings property, if it's set in the _tempProfile
        // then the user opened the edit key bindings dialog and clicked
        // OK, and could have add/removed a key bindings rule
        } else if (property == Profile::KeyBindings || (value != _profile->property<QVariant>(property))) {
            userModified = true;
            break;
        }
    }

    enableButtonApply(userModified);
}

void EditProfileDialog::setupKeyboardPage(const Profile::Ptr /* profile */)
{
    // setup translator list
    updateKeyBindingsList(lookupProfile()->keyBindings());

    connect(_ui->keyBindingList->selectionModel(),
            &QItemSelectionModel::selectionChanged,
            this, &Konsole::EditProfileDialog::keyBindingSelected);
    connect(_ui->newKeyBindingsButton, &QPushButton::clicked, this,
            &Konsole::EditProfileDialog::newKeyBinding);

    _ui->editKeyBindingsButton->setEnabled(false);
    _ui->removeKeyBindingsButton->setEnabled(false);
    _ui->resetKeyBindingsButton->setEnabled(false);

    updateKeyBindingsButtons();

    connect(_ui->editKeyBindingsButton, &QPushButton::clicked, this,
            &Konsole::EditProfileDialog::editKeyBinding);
    connect(_ui->removeKeyBindingsButton, &QPushButton::clicked, this,
            &Konsole::EditProfileDialog::removeKeyBinding);
    connect(_ui->resetKeyBindingsButton, &QPushButton::clicked, this,
            &Konsole::EditProfileDialog::resetKeyBindings);
}
void EditProfileDialog::keyBindingSelected()
{
    QModelIndexList selected = _ui->keyBindingList->selectionModel()->selectedIndexes();

    if (!selected.isEmpty()) {
        QAbstractItemModel* model = _ui->keyBindingList->model();
        const KeyboardTranslator* translator = model->data(selected.first(), Qt::UserRole + 1)
                                               .value<const KeyboardTranslator*>();
        if (translator) {
            updateTempProfileProperty(Profile::KeyBindings, translator->name());
        }
    }

    updateKeyBindingsButtons();
}
void EditProfileDialog::removeKeyBinding()
{
    QModelIndexList selected = _ui->keyBindingList->selectionModel()->selectedIndexes();

    if (!selected.isEmpty()) {
        const QString& name = selected.first().data(Qt::UserRole + 1).value<const KeyboardTranslator*>()->name();
        if (KeyboardTranslatorManager::instance()->deleteTranslator(name))
            _ui->keyBindingList->model()->removeRow(selected.first().row());
    }
}
void EditProfileDialog::showKeyBindingEditor(bool isNewTranslator)
{
    QModelIndexList selected = _ui->keyBindingList->selectionModel()->selectedIndexes();
    QAbstractItemModel* model = _ui->keyBindingList->model();

    const KeyboardTranslator* translator = nullptr;
    if (!selected.isEmpty())
        translator = model->data(selected.first(), Qt::UserRole + 1).value<const KeyboardTranslator*>();
    else
        translator = _keyManager->defaultTranslator();

    Q_ASSERT(translator);

    auto editor = new KeyBindingEditor(this);

    if (translator)
        editor->setup(translator, lookupProfile()->keyBindings(), isNewTranslator);

    connect(editor, &Konsole::KeyBindingEditor::updateKeyBindingsListRequest,
            this, &Konsole::EditProfileDialog::updateKeyBindingsList);


    connect(editor, &Konsole::KeyBindingEditor::updateTempProfileKeyBindingsRequest,
            this, &Konsole::EditProfileDialog::updateTempProfileProperty);

    editor->exec();
}
void EditProfileDialog::newKeyBinding()
{
    showKeyBindingEditor(true);
}
void EditProfileDialog::editKeyBinding()
{
    showKeyBindingEditor(false);
}

void EditProfileDialog::resetKeyBindings()
{
    QModelIndexList selected = _ui->keyBindingList->selectionModel()->selectedIndexes();

    if (!selected.isEmpty()) {
        const QString &name = selected.first().data(Qt::UserRole + 1).value<const KeyboardTranslator *>()->name();

        _keyManager->deleteTranslator(name);
        // find and load the translator
        _keyManager->findTranslator(name);

        updateKeyBindingsList(name);
    }
}

void EditProfileDialog::setupCheckBoxes(const QVector<BooleanOption>& options , const Profile::Ptr profile)
{
    for(const auto& option : options) {
        option.button->setChecked(profile->property<bool>(option.property));
        connect(option.button, SIGNAL(toggled(bool)), this, option.slot);
    }
}
void EditProfileDialog::setupRadio(const QVector<RadioOption>& possibilities, int actual)
{
    for(const auto& possibility : possibilities) {
        possibility.button->setChecked(possibility.value == actual);
        connect(possibility.button, SIGNAL(clicked()), this, possibility.slot);
    }
}

void EditProfileDialog::setupScrollingPage(const Profile::Ptr profile)
{
    // setup scrollbar radio
    int scrollBarPosition = profile->property<int>(Profile::ScrollBarPosition);

    const auto positions = QVector<RadioOption>{ {_ui->scrollBarHiddenButton, Enum::ScrollBarHidden, SLOT(hideScrollBar())},
        {_ui->scrollBarLeftButton, Enum::ScrollBarLeft, SLOT(showScrollBarLeft())},
        {_ui->scrollBarRightButton, Enum::ScrollBarRight, SLOT(showScrollBarRight())}
    };

    setupRadio(positions , scrollBarPosition);

    // setup scrollback type radio
    int scrollBackType = profile->property<int>(Profile::HistoryMode);
    _ui->historySizeWidget->setMode(Enum::HistoryModeEnum(scrollBackType));
    connect(_ui->historySizeWidget, &Konsole::HistorySizeWidget::historyModeChanged,
            this, &Konsole::EditProfileDialog::historyModeChanged);

    // setup scrollback line count spinner
    const int historySize = profile->historySize();
    _ui->historySizeWidget->setLineCount(historySize);

    // setup scrollpageamount type radio
    int scrollFullPage = profile->property<int>(Profile::ScrollFullPage);

    const auto pageamounts = QVector<RadioOption>{
        {_ui->scrollHalfPage, Enum::ScrollPageHalf, SLOT(scrollHalfPage())},
        {_ui->scrollFullPage, Enum::ScrollPageFull, SLOT(scrollFullPage())}
    };

    setupRadio(pageamounts, scrollFullPage);

    // signals and slots
    connect(_ui->historySizeWidget, &Konsole::HistorySizeWidget::historySizeChanged,
            this, &Konsole::EditProfileDialog::historySizeChanged);
}

void EditProfileDialog::historySizeChanged(int lineCount)
{
    updateTempProfileProperty(Profile::HistorySize , lineCount);
}
void EditProfileDialog::historyModeChanged(Enum::HistoryModeEnum mode)
{
    updateTempProfileProperty(Profile::HistoryMode, mode);
}
void EditProfileDialog::hideScrollBar()
{
    updateTempProfileProperty(Profile::ScrollBarPosition, Enum::ScrollBarHidden);
}
void EditProfileDialog::showScrollBarLeft()
{
    updateTempProfileProperty(Profile::ScrollBarPosition, Enum::ScrollBarLeft);
}
void EditProfileDialog::showScrollBarRight()
{
    updateTempProfileProperty(Profile::ScrollBarPosition, Enum::ScrollBarRight);
}
void EditProfileDialog::scrollFullPage()
{
    updateTempProfileProperty(Profile::ScrollFullPage, Enum::ScrollPageFull);
}
void EditProfileDialog::scrollHalfPage()
{
    updateTempProfileProperty(Profile::ScrollFullPage, Enum::ScrollPageHalf);
}
void EditProfileDialog::setupMousePage(const Profile::Ptr profile)
{
    const auto options = QVector<BooleanOption>{
        {
            _ui->underlineLinksButton , Profile::UnderlineLinksEnabled,
            SLOT(toggleUnderlineLinks(bool))
        },
        {
            _ui->ctrlRequiredForDragButton, Profile::CtrlRequiredForDrag,
            SLOT(toggleCtrlRequiredForDrag(bool))
        },
        {
            _ui->copyTextToClipboardButton , Profile::AutoCopySelectedText,
            SLOT(toggleCopyTextToClipboard(bool))
        },
        {
            _ui->trimLeadingSpacesButton, Profile::TrimLeadingSpacesInSelectedText,
            SLOT(toggleTrimLeadingSpacesInSelectedText(bool))
        },
        {
            _ui->trimTrailingSpacesButton , Profile::TrimTrailingSpacesInSelectedText,
            SLOT(toggleTrimTrailingSpacesInSelectedText(bool))
        },
        {
            _ui->openLinksByDirectClickButton , Profile::OpenLinksByDirectClickEnabled,
            SLOT(toggleOpenLinksByDirectClick(bool))
        },
        {
            _ui->enableAlternateScrollingButton, Profile::AlternateScrolling,
            SLOT(toggleAlternateScrolling(bool))
        }
    };
    setupCheckBoxes(options , profile);

    // setup middle click paste mode
    const int middleClickPasteMode = profile->property<int>(Profile::MiddleClickPasteMode);
    const auto pasteModes = QVector<RadioOption> {
        {_ui->pasteFromX11SelectionButton, Enum::PasteFromX11Selection, SLOT(pasteFromX11Selection())},
        {_ui->pasteFromClipboardButton, Enum::PasteFromClipboard, SLOT(pasteFromClipboard())}
    };
    setupRadio(pasteModes , middleClickPasteMode);

    // interaction options
    _ui->wordCharacterEdit->setText(profile->wordCharacters());

    connect(_ui->wordCharacterEdit, &QLineEdit::textChanged, this,
            &Konsole::EditProfileDialog::wordCharactersChanged);

    int tripleClickMode = profile->property<int>(Profile::TripleClickMode);
    _ui->tripleClickModeCombo->setCurrentIndex(tripleClickMode);

    connect(_ui->tripleClickModeCombo, static_cast<void(KComboBox::*)(int)>(&KComboBox::activated), this,
            &Konsole::EditProfileDialog::TripleClickModeChanged);

    _ui->openLinksByDirectClickButton->setEnabled(_ui->underlineLinksButton->isChecked());

    _ui->enableMouseWheelZoomButton->setChecked(profile->mouseWheelZoomEnabled());
    connect(_ui->enableMouseWheelZoomButton, &QCheckBox::toggled, this,
            &Konsole::EditProfileDialog::toggleMouseWheelZoom);
}
void EditProfileDialog::setupAdvancedPage(const Profile::Ptr profile)
{
    const auto options = QVector<BooleanOption>{
        {
            _ui->enableBlinkingTextButton , Profile::BlinkingTextEnabled ,
            SLOT(toggleBlinkingText(bool))
        },
        {
            _ui->enableFlowControlButton , Profile::FlowControlEnabled ,
            SLOT(toggleFlowControl(bool))
        },
        {
            _ui->enableBlinkingCursorButton , Profile::BlinkingCursorEnabled ,
            SLOT(toggleBlinkingCursor(bool))
        },
        {
            _ui->enableBidiRenderingButton , Profile::BidiRenderingEnabled ,
            SLOT(togglebidiRendering(bool))
        }
    };
    setupCheckBoxes(options , profile);

    const int lineSpacing = profile->lineSpacing();
    _ui->lineSpacingSpinner->setValue(lineSpacing);

    connect(_ui->lineSpacingSpinner, static_cast<void(KIntSpinBox::*)(int)>(&KIntSpinBox::valueChanged),
            this, &Konsole::EditProfileDialog::lineSpacingChanged);

    // cursor options
    if (profile->useCustomCursorColor())
        _ui->customCursorColorButton->setChecked(true);
    else
        _ui->autoCursorColorButton->setChecked(true);

    _ui->customColorSelectButton->setColor(profile->customCursorColor());

    connect(_ui->customCursorColorButton, &QRadioButton::clicked, this, &Konsole::EditProfileDialog::customCursorColor);
    connect(_ui->autoCursorColorButton, &QRadioButton::clicked, this, &Konsole::EditProfileDialog::autoCursorColor);
    connect(_ui->customColorSelectButton, &KColorButton::changed,
            this, &Konsole::EditProfileDialog::customCursorColorChanged);

    int shape = profile->property<int>(Profile::CursorShape);
    _ui->cursorShapeCombo->setCurrentIndex(shape);

    connect(_ui->cursorShapeCombo, static_cast<void(KComboBox::*)(int)>(&KComboBox::activated), this, &Konsole::EditProfileDialog::setCursorShape);

    // encoding options
    KCodecAction* codecAction = new KCodecAction(this);
    _ui->selectEncodingButton->setMenu(codecAction->menu());
    connect(codecAction, static_cast<void(KCodecAction::*)(QTextCodec*)>(&KCodecAction::triggered), this, &Konsole::EditProfileDialog::setDefaultCodec);

    _ui->characterEncodingLabel->setText(profile->defaultEncoding());
}
void EditProfileDialog::setDefaultCodec(QTextCodec* codec)
{
    QString name = QString::fromLocal8Bit(codec->name());

    updateTempProfileProperty(Profile::DefaultEncoding, name);
    _ui->characterEncodingLabel->setText(name);
}
void EditProfileDialog::customCursorColorChanged(const QColor& color)
{
    updateTempProfileProperty(Profile::CustomCursorColor, color);

    // ensure that custom cursor colors are enabled
    _ui->customCursorColorButton->click();
}
void EditProfileDialog::wordCharactersChanged(const QString& text)
{
    updateTempProfileProperty(Profile::WordCharacters, text);
}
void EditProfileDialog::autoCursorColor()
{
    updateTempProfileProperty(Profile::UseCustomCursorColor, false);
}
void EditProfileDialog::customCursorColor()
{
    updateTempProfileProperty(Profile::UseCustomCursorColor, true);
}
void EditProfileDialog::setCursorShape(int index)
{
    updateTempProfileProperty(Profile::CursorShape, index);
}
void EditProfileDialog::togglebidiRendering(bool enable)
{
    updateTempProfileProperty(Profile::BidiRenderingEnabled, enable);
}
void EditProfileDialog::lineSpacingChanged(int spacing)
{
    updateTempProfileProperty(Profile::LineSpacing, spacing);
}
void EditProfileDialog::toggleBlinkingCursor(bool enable)
{
    updateTempProfileProperty(Profile::BlinkingCursorEnabled, enable);
}
void EditProfileDialog::toggleUnderlineLinks(bool enable)
{
    updateTempProfileProperty(Profile::UnderlineLinksEnabled, enable);
    _ui->openLinksByDirectClickButton->setEnabled(enable);
}
void EditProfileDialog::toggleCtrlRequiredForDrag(bool enable)
{
    updateTempProfileProperty(Profile::CtrlRequiredForDrag, enable);
}
void EditProfileDialog::toggleOpenLinksByDirectClick(bool enable)
{
    updateTempProfileProperty(Profile::OpenLinksByDirectClickEnabled, enable);
}
void EditProfileDialog::toggleCopyTextToClipboard(bool enable)
{
    updateTempProfileProperty(Profile::AutoCopySelectedText, enable);
}

void EditProfileDialog::toggleTrimLeadingSpacesInSelectedText(bool enable)
{
    updateTempProfileProperty(Profile::TrimLeadingSpacesInSelectedText, enable);
}

void EditProfileDialog::toggleTrimTrailingSpacesInSelectedText(bool enable)
{
    updateTempProfileProperty(Profile::TrimTrailingSpacesInSelectedText, enable);
}
void EditProfileDialog::pasteFromX11Selection()
{
    updateTempProfileProperty(Profile::MiddleClickPasteMode, Enum::PasteFromX11Selection);
}
void EditProfileDialog::pasteFromClipboard()
{
    updateTempProfileProperty(Profile::MiddleClickPasteMode, Enum::PasteFromClipboard);
}
void EditProfileDialog::TripleClickModeChanged(int newValue)
{
    updateTempProfileProperty(Profile::TripleClickMode, newValue);
}
void EditProfileDialog::toggleBlinkingText(bool enable)
{
    updateTempProfileProperty(Profile::BlinkingTextEnabled, enable);
}
void EditProfileDialog::toggleFlowControl(bool enable)
{
    updateTempProfileProperty(Profile::FlowControlEnabled, enable);
}
void EditProfileDialog::fontSelected(const QFont& aFont)
{
    QFont previewFont = aFont;

    setFontInputValue(aFont);

    _ui->fontPreviewLabel->setFont(previewFont);

    preview(Profile::Font, aFont);
    updateTempProfileProperty(Profile::Font, aFont);
}
void EditProfileDialog::showFontDialog()
{
    QString sampleText = QLatin1String("ell 'lL', one '1', little eye 'i', big eye");
    sampleText += QLatin1String("'I', lL1iI, Zero '0', little oh 'o', big oh 'O', 0oO");
    sampleText += QLatin1String("`~!@#$%^&*()_+-=[]\\{}|:\";'<>?,./");
    sampleText += QLatin1String("0123456789");
    sampleText += QLatin1String("\nThe Quick Brown Fox Jumps Over The Lazy Dog\n");
    sampleText += i18n("--- Type anything in this box ---");
    QFont currentFont = _ui->fontPreviewLabel->font();

    QWeakPointer<KFontDialog> dialog = new KFontDialog(this, KFontChooser::FixedFontsOnly);
    dialog.data()->setCaption(i18n("Select Fixed Width Font"));
    dialog.data()->setFont(currentFont, true);

    // TODO (hindenburg): When https://git.reviewboard.kde.org/r/103357 is
    // committed, change the below.
    // Use text more fitting to show font differences in a terminal
    QList<KFontChooser*> chooserList = dialog.data()->findChildren<KFontChooser*>();
    if (!chooserList.isEmpty())
        chooserList.at(0)->setSampleText(sampleText);

    connect(dialog.data(), &KFontDialog::fontSelected, this, &Konsole::EditProfileDialog::fontSelected);

    if (dialog.data()->exec() == QDialog::Rejected)
        fontSelected(currentFont);
    delete dialog.data();
}
void EditProfileDialog::setFontSize(double pointSize)
{
    QFont newFont = _ui->fontPreviewLabel->font();
    newFont.setPointSizeF(pointSize);
    _ui->fontPreviewLabel->setFont(newFont);

    preview(Profile::Font, newFont);
    updateTempProfileProperty(Profile::Font, newFont);
}

void EditProfileDialog::setFontInputValue(const QFont& font)
{
    _ui->fontSizeInput->setValue(font.pointSizeF());
}

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

#include "EditProfileDialog.moc"
