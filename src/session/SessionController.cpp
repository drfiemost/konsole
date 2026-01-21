/*
    Copyright 2006-2008 by Robert Knight <robertknight@gmail.com>
    Copyright 2009 by Thomas Dreibholz <dreibh@iem.uni-due.de>

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
#include "session/SessionController.h"

// Qt
#include <QApplication>
#include <QList>
#include <QMenu>
#include <QKeyEvent>
#include <QPrinter>
#include <QPrintDialog>
#include <QPainter>
#include <QDir>

// KDE
#include <KAction>
#include <KActionMenu>
#include <KActionCollection>
#include <KIcon>
#include <KLocalizedString>
#include <KMenu>
#include <KMessageBox>
#include <KRun>
#include <KShell>
#include <KToolInvocation>
#include <KStandardDirs>
#include <KToggleAction>
#include <KSelectAction>
#include <KUrl>
#include <KXmlGuiWindow>
#include <KXMLGUIFactory>
#include <KXMLGUIBuilder>
#include <KDebug>
#include <KUriFilter>
#include <KStringHandler>
#include <KConfigGroup>
#include <KGlobal>
#include <KCodecAction>
#include <KFileDialog>
#include <KNotification>

// Konsole
#include "profile/EditProfileDialog.h"
#include "CopyInputDialog.h"
#include "Emulation.h"
#include "filterHotSpots/Filter.h"
#include "filterHotSpots/FilterChain.h"
#include "filterHotSpots/HotSpot.h"
#include "filterHotSpots/RegExpFilter.h"
#include "filterHotSpots/UrlFilter.h"
#include "history/HistoryType.h"
#include "history/HistoryTypeNone.h"
#include "history/HistoryTypeFile.h"
#include "history/compact/CompactHistoryType.h"
#include "HistorySizeDialog.h"
#include "widgets/IncrementalSearchBar.h"
#include "RenameTabDialog.h"
#include "ScreenWindow.h"
#include "Session.h"
#include "profile/ProfileList.h"
#include "widgets/TerminalDisplay.h"
#include "SessionManager.h"
#include "Enumeration.h"
#include "PrintOptions.h"

#include "SaveHistoryTask.h"
#include "SearchHistoryTask.h"
#include "SessionGroup.h"

// For Unix signal names
#include <csignal>

#include <utility>

using namespace Konsole;

// TODO - Replace the icon choices below when suitable icons for silence and
// activity are available
const KIcon SessionController::_activityIcon(QLatin1String("dialog-information"));
const KIcon SessionController::_silenceIcon(QLatin1String("dialog-information"));
const KIcon SessionController::_bellIcon(QLatin1String("preferences-desktop-notification-bell"));
const KIcon SessionController::_broadcastIcon(QLatin1String("emblem-important"));

QSet<SessionController*> SessionController::_allControllers;
int SessionController::_lastControllerId;

SessionController::SessionController(Session* session, TerminalDisplay* view, QObject* parent)
    : ViewProperties(parent)
    , KXMLGUIClient()
    , _copyToGroup(nullptr)
    , _profileList(nullptr)
    , _searchFilter(nullptr)
    , _copyInputToAllTabsAction(nullptr)
    , _findAction(nullptr)
    , _findNextAction(nullptr)
    , _findPreviousAction(nullptr)
    , _urlFilterUpdateRequired(false)
    , _searchStartLine(0)
    , _prevSearchResultLine(0)
    , _codecAction(nullptr)
    , _switchProfileMenu(nullptr)
    , _webSearchMenu(nullptr)
    , _listenForScreenWindowUpdates(false)
    , _preventClose(false)
    , _keepIconUntilInteraction(false)
    , _showMenuAction(nullptr)
    , _isSearchBarEnabled(false)
    , _searchBar(view->searchBar())
    , _monitorProcessFinish(false)
{
    Q_ASSERT(session);
    Q_ASSERT(view);

    _sessionDisplayConnection = new SessionDisplayConnection(session, view, this);

    // handle user interface related to session (menus etc.)
    if (isKonsolePart()) {
        setXMLFile(QStringLiteral("konsole/partui.rc"));
        setupCommonActions();
    } else {
        setXMLFile(QStringLiteral("konsole/sessionui.rc"));
        setupCommonActions();
        setupExtraActions();
    }

    actionCollection()->addAssociatedWidget(view);
    const QList<QAction *> actionsList = actionCollection()->actions();
    for (QAction *action: actionsList) {
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    }

    setIdentifier(++_lastControllerId);
    sessionAttributeChanged();

    view->installEventFilter(this);
    view->setSessionController(this);

    // install filter on the view to highlight URLs
    _sessionDisplayConnection->view()->filterChain()->addFilter(new UrlFilter);

    // listen for session resize requests
    connect(_sessionDisplayConnection->session(), &Konsole::Session::resizeRequest, this,
            &Konsole::SessionController::sessionResizeRequest);

    // listen for popup menu requests
    connect(_sessionDisplayConnection->view(), &Konsole::TerminalDisplay::configureRequest, this,
            &Konsole::SessionController::showDisplayContextMenu);

    // move view to newest output when keystrokes occur
    connect(_sessionDisplayConnection->view(), &Konsole::TerminalDisplay::keyPressedSignal, this,
            &Konsole::SessionController::trackOutput);

    // listen to activity / silence notifications from session
    connect(_sessionDisplayConnection->session(), &Konsole::Session::notificationsChanged, this,
            &Konsole::SessionController::sessionNotificationsChanged);
    // listen to title and icon changes
    connect(_sessionDisplayConnection->session(), &Konsole::Session::sessionAttributeChanged, this, &Konsole::SessionController::sessionAttributeChanged);

    connect(this, &Konsole::SessionController::tabRenamedByUser,  _sessionDisplayConnection->session(),  &Konsole::Session::tabRenamedByUser);

    connect(_sessionDisplayConnection->session(), &Konsole::Session::currentDirectoryChanged ,
            this , &Konsole::SessionController::currentDirectoryChanged);

    // listen for color changes
    connect(_sessionDisplayConnection->session(), &Konsole::Session::changeBackgroundColorRequest, _sessionDisplayConnection->view().data(), &Konsole::TerminalDisplay::setBackgroundColor);
    connect(_sessionDisplayConnection->session(), &Konsole::Session::changeForegroundColorRequest, _sessionDisplayConnection->view().data(), &Konsole::TerminalDisplay::setForegroundColor);

    // update the title when the session starts
    connect(_sessionDisplayConnection->session(), &Konsole::Session::started, this, &Konsole::SessionController::snapshot);

    // listen for output changes to set activity flag
    connect(_sessionDisplayConnection->session()->emulation(), &Konsole::Emulation::outputChanged, this,
            &Konsole::SessionController::fireActivity);

    // listen for detection of ZModem transfer
    connect(_sessionDisplayConnection->session(), &Konsole::Session::zmodemDownloadDetected, this, &Konsole::SessionController::zmodemDownload);
    connect(_sessionDisplayConnection->session(), &Konsole::Session::zmodemUploadDetected, this, &Konsole::SessionController::zmodemUpload);

    // listen for flow control status changes
    connect(_sessionDisplayConnection->session(), &Konsole::Session::flowControlEnabledChanged, _sessionDisplayConnection->view().data(),
            &Konsole::TerminalDisplay::setFlowControlWarningEnabled);
    _sessionDisplayConnection->view()->setFlowControlWarningEnabled(_sessionDisplayConnection->session()->flowControlEnabled());

    // take a snapshot of the session state every so often when
    // user activity occurs
    //
    // the timer is owned by the session so that it will be destroyed along
    // with the session
    _interactionTimer = new QTimer(_sessionDisplayConnection->session());
    _interactionTimer->setSingleShot(true);
    _interactionTimer->setInterval(500);
    connect(_interactionTimer, &QTimer::timeout, this, &Konsole::SessionController::snapshot);
    connect(_sessionDisplayConnection->view(), &Konsole::TerminalDisplay::keyPressedSignal, this, &Konsole::SessionController::interactionHandler);

    // take a snapshot of the session state periodically in the background
    QTimer* backgroundTimer = new QTimer(_sessionDisplayConnection->session());
    backgroundTimer->setSingleShot(false);
    backgroundTimer->setInterval(2000);
    connect(backgroundTimer, &QTimer::timeout, this, &Konsole::SessionController::snapshot);
    backgroundTimer->start();

    // xterm '10;?' request
    connect(_sessionDisplayConnection->session(), &Konsole::Session::getForegroundColor,
            this, &Konsole::SessionController::sendForegroundColor);
    // xterm '11;?' request
    connect(_sessionDisplayConnection->session(), &Konsole::Session::getBackgroundColor,
            this, &Konsole::SessionController::sendBackgroundColor);

    _allControllers.insert(this);

    // A list of programs that accept Ctrl+C to clear command line used
    // before outputting bookmark.
    _bookmarkValidProgramsToClear = QStringList({
        QStringLiteral("bash"),
        QStringLiteral("fish"),
        QStringLiteral("sh"),
        QStringLiteral("tcsh"),
        QStringLiteral("zsh")
    });
    setupSearchBar();
    _searchBar->setVisible(_isSearchBarEnabled);
}

SessionController::~SessionController()
{
    if (!_sessionDisplayConnection->view().isNull())
        _sessionDisplayConnection->view()->setScreenWindow(nullptr);
    delete _sessionDisplayConnection;

    _allControllers.remove(this);

    if (!_editProfileDialog.isNull()) {
        _editProfileDialog->deleteLater();
    }
    if(factory()) {
        factory()->removeClient(this);
    }
}
void SessionController::trackOutput(QKeyEvent* event)
{
    Q_ASSERT(_sessionDisplayConnection->view()->screenWindow());

    // Only jump to the bottom if the user actually typed something in,
    // not if the user e. g. just pressed a modifier.
    if (event->text().isEmpty() && (event->modifiers() != 0u)) {
        return;
    }

    _sessionDisplayConnection->view()->screenWindow()->setTrackOutput(true);
}
void SessionController::interactionHandler()
{
    // This flag is used to make sure those special icons indicating interest
    // events (activity/silence/bell?) remain in the tab until user interaction
    // happens. Otherwise, those special icons will quickly be replaced by
    // normal icon when ::snapshot() is triggered
    _keepIconUntilInteraction = false;
    _interactionTimer->start();
}

void SessionController::requireUrlFilterUpdate()
{
    // this method is called every time the screen window's output changes, so do not
    // do anything expensive here.

    _urlFilterUpdateRequired = true;
}
void SessionController::snapshot()
{
    Q_ASSERT(!_sessionDisplayConnection->session().isNull());

    QString title = _sessionDisplayConnection->session()->getDynamicTitle();
    title         = title.simplified();

    // Visualize that the session is broadcasting to others
    if (_copyToGroup && _copyToGroup->sessions().count() > 1) {
        title.append(QLatin1Char('*'));
    }

    // use the fallback title if needed
    if (title.isEmpty()) {
        title = _sessionDisplayConnection->session()->title(Session::NameRole);
    }

    // apply new title
    _sessionDisplayConnection->session()->setTitle(Session::DisplayedTitleRole, title);

    // check if foreground process ended and notify if this option was requested
    if (_monitorProcessFinish) {
        bool isForegroundProcessActive = _sessionDisplayConnection->session()->isForegroundProcessActive();
        if (!_previousForegroundProcessName.isNull() && !isForegroundProcessActive) {
            KNotification::event(_sessionDisplayConnection->session()->hasFocus() ? QStringLiteral("ProcessFinished") : QStringLiteral("ProcessFinishedHidden"),
                                 i18n("The process '%1' has finished running in session '%2'", _previousForegroundProcessName, _sessionDisplayConnection->session()->nameTitle()),
                                 QPixmap(),
                                 QApplication::activeWindow(),
                                 KNotification::CloseWhenWidgetActivated);
        }
        _previousForegroundProcessName = isForegroundProcessActive ? _sessionDisplayConnection->session()->foregroundProcessName() : QString();
    }

    // do not forget icon
    updateSessionIcon();
}

QString SessionController::currentDir() const
{
    return _sessionDisplayConnection->session()->currentWorkingDirectory();
}

KUrl SessionController::url() const
{
    return _sessionDisplayConnection->session()->getUrl();
}

void SessionController::rename()
{
    renameSession();
}

void SessionController::openUrl(const KUrl& url)
{
    // Clear shell's command line
    if (!_sessionDisplayConnection->session()->isForegroundProcessActive()
            && _bookmarkValidProgramsToClear.contains(_sessionDisplayConnection->session()->foregroundProcessName())) {
        _sessionDisplayConnection->session()->sendTextToTerminal(QChar(0x03), QLatin1Char('\n')); // Ctrl+C
    }

    // handle local paths
    if (url.isLocalFile()) {
        QString path = url.toLocalFile();
        _sessionDisplayConnection->session()->sendTextToTerminal(QStringLiteral("cd ") + KShell::quoteArg(path), QLatin1Char('\r'));
    } else if (url.protocol().isEmpty()) {
        // KUrl couldn't parse what the user entered into the URL field
        // so just dump it to the shell
        QString command = url.prettyUrl();
        if (!command.isEmpty())
            _sessionDisplayConnection->session()->sendTextToTerminal(command, QLatin1Char('\r'));
    } else if (url.protocol() == QLatin1String("ssh")) {
        QString sshCommand = QStringLiteral("ssh ");

        if (url.port() > -1) {
            sshCommand += QStringLiteral("-p %1 ").arg(url.port());
        }
        if (url.hasUser()) {
            sshCommand += (url.user() + QLatin1Char('@'));
        }
        if (url.hasHost()) {
            sshCommand += url.host();
        }

        _sessionDisplayConnection->session()->sendTextToTerminal(sshCommand, QLatin1Char('\r'));

    } else if (url.protocol() == QLatin1String("telnet")) {
        QString telnetCommand = QStringLiteral("telnet ");

        if (url.hasUser()) {
            telnetCommand += QStringLiteral("-l %1 ").arg(url.user());
        }
        if (url.hasHost()) {
            telnetCommand += (url.host() + QLatin1Char(' '));
        }
        if (url.port() > -1) {
            telnetCommand += QString::number(url.port());
        }

        _sessionDisplayConnection->session()->sendTextToTerminal(telnetCommand, QLatin1Char('\r'));

    } else {
        //TODO Implement handling for other Url types

        KMessageBox::sorry(_sessionDisplayConnection->view()->window(),
                           i18n("Konsole does not know how to open the bookmark: ") +
                           url.prettyUrl());

        kWarning() << "Unable to open bookmark at url" << url << ", I do not know"
                   << " how to handle the protocol " << url.protocol();
    }
}

void SessionController::setupPrimaryScreenSpecificActions(bool use)
{
    KActionCollection* collection = actionCollection();
    QAction* clearAction = collection->action(QStringLiteral("clear-history"));
    QAction* resetAction = collection->action(QStringLiteral("clear-history-and-reset"));
    QAction* selectAllAction = collection->action(QStringLiteral("select-all"));
    QAction* selectLineAction = collection->action(QStringLiteral("select-line"));

    // these actions are meaningful only when primary screen is used.
    clearAction->setEnabled(use);
    resetAction->setEnabled(use);
    selectAllAction->setEnabled(use);
    selectLineAction->setEnabled(use);
}

void SessionController::selectionChanged(const QString& selectedText)
{
    _selectedText = selectedText;
    updateCopyAction(selectedText);
}

void SessionController::updateCopyAction(const QString& selectedText)
{
    QAction* copyAction = actionCollection()->action(QStringLiteral("edit_copy"));

    // copy action is meaningful only when some text is selected.
    copyAction->setEnabled(!selectedText.isEmpty());
}

void SessionController::updateWebSearchMenu()
{
    // reset
    _webSearchMenu->setVisible(false);
    _webSearchMenu->menu()->clear();

    if (_selectedText.isEmpty())
        return;

    QString searchText = _selectedText;
    searchText = searchText.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' ')).simplified();

    if (searchText.isEmpty())
        return;

    KUriFilterData filterData(searchText);
    filterData.setSearchFilteringOptions(KUriFilterData::RetrievePreferredSearchProvidersOnly);

    if (KUriFilter::self()->filterSearchUri(filterData, KUriFilter::NormalTextFilter)) {
        const QStringList searchProviders = filterData.preferredSearchProviders();
        if (!searchProviders.isEmpty()) {
            _webSearchMenu->setText(i18n("Search for '%1' with",  KStringHandler::rsqueeze(searchText, 16)));

            KAction* action = nullptr;

            for (const QString& searchProvider: searchProviders) {
                action = new KAction(searchProvider, _webSearchMenu);
                action->setIcon(KIcon(filterData.iconNameForPreferredSearchProvider(searchProvider)));
                action->setData(filterData.queryForPreferredSearchProvider(searchProvider));
                connect(action, &KAction::triggered, this, &Konsole::SessionController::handleWebShortcutAction);
                _webSearchMenu->addAction(action);
            }

            _webSearchMenu->addSeparator();

            action = new KAction(i18n("Configure Web Shortcuts..."), _webSearchMenu);
            action->setIcon(KIcon(QStringLiteral("configure")));
            connect(action, &KAction::triggered, this, &Konsole::SessionController::configureWebShortcuts);
            _webSearchMenu->addAction(action);

            _webSearchMenu->setVisible(true);
        }
    }
}

void SessionController::handleWebShortcutAction()
{
    KAction* action = qobject_cast<KAction*>(sender());
    if (!action)
        return;

    KUriFilterData filterData(action->data().toString());

    if (KUriFilter::self()->filterUri(filterData, { QStringLiteral("kurisearchfilter") })) {
        const KUrl& url = filterData.uri();
        new KRun(url, QApplication::activeWindow());
    }
}

void SessionController::configureWebShortcuts()
{
    KToolInvocation::kdeinitExec(QStringLiteral("kcmshell4"), { QStringLiteral("ebrowsing") });
}

void SessionController::sendSignal(QAction* action)
{
    const int signal = action->data().toInt();
    _sessionDisplayConnection->session()->sendSignal(signal);
}

void SessionController::sendForegroundColor()
{
    const QColor c = _sessionDisplayConnection->view()->getForegroundColor();
    _sessionDisplayConnection->session()->reportForegroundColor(c);
}

void SessionController::sendBackgroundColor()
{
    const QColor c = _sessionDisplayConnection->view()->getBackgroundColor();
    _sessionDisplayConnection->session()->reportBackgroundColor(c);
}

bool SessionController::eventFilter(QObject* watched , QEvent* event)
{
    if (event->type() == QEvent::FocusIn && watched == _sessionDisplayConnection->view()) {
        // notify the world that the view associated with this session has been focused
        // used by the view manager to update the title of the MainWindow widget containing the view
        emit focused(this);

        // when the view is focused, set bell events from the associated session to be delivered
        // by the focused view

        // first, disconnect any other views which are listening for bell signals from the session
        disconnect(_sessionDisplayConnection->session(), &Konsole::Session::bellRequest, nullptr, nullptr);
        // second, connect the newly focused view to listen for the session's bell signal
        connect(_sessionDisplayConnection->session(), &Konsole::Session::bellRequest,
                    _sessionDisplayConnection->view().data(), &Konsole::TerminalDisplay::bell);

        if (_copyInputToAllTabsAction && _copyInputToAllTabsAction->isChecked()) {
            // A session with "Copy To All Tabs" has come into focus:
            // Ensure that newly created sessions are included in _copyToGroup!
            copyInputToAllTabs();
        }
    }

    return Konsole::ViewProperties::eventFilter(watched, event);
}

void SessionController::removeSearchFilter()
{
    if (!_searchFilter)
        return;

    _sessionDisplayConnection->view()->filterChain()->removeFilter(_searchFilter);
    delete _searchFilter;
    _searchFilter = nullptr;
}

void SessionController::setupSearchBar()
{
    connect(_searchBar, &Konsole::IncrementalSearchBar::unhandledMovementKeyPressed, this, &Konsole::SessionController::movementKeyFromSearchBarReceived);
    connect(_searchBar, &Konsole::IncrementalSearchBar::closeClicked, this, &Konsole::SessionController::searchClosed);
    connect(_searchBar, &Konsole::IncrementalSearchBar::searchFromClicked, this, &Konsole::SessionController::searchFrom);
    connect(_searchBar, &Konsole::IncrementalSearchBar::findNextClicked, this, &Konsole::SessionController::findNextInHistory);
    connect(_searchBar, &Konsole::IncrementalSearchBar::findPreviousClicked, this, &Konsole::SessionController::findPreviousInHistory);
    connect(_searchBar, &Konsole::IncrementalSearchBar::highlightMatchesToggled , this , &Konsole::SessionController::highlightMatches);
    connect(_searchBar, &Konsole::IncrementalSearchBar::matchCaseToggled, this, &Konsole::SessionController::changeSearchMatch);
    connect(_searchBar, &Konsole::IncrementalSearchBar::matchRegExpToggled, this, &Konsole::SessionController::changeSearchMatch);
}

void SessionController::setShowMenuAction(QAction* action)
{
    _showMenuAction = action;
}

void SessionController::setupCommonActions()
{
    KActionCollection* collection = actionCollection();

    // Close Session
    KAction* action = collection->addAction(QStringLiteral("close-session"), this, SLOT(closeSession()));
    if (isKonsolePart())
        action->setText(i18n("&Close Session"));
    else
        action->setText(i18n("&Close Tab"));

    action->setIcon(KIcon(QStringLiteral("tab-close")));
    action->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_W));

    // Open Browser
    action = collection->addAction(QStringLiteral("open-browser"), this, SLOT(openBrowser()));
    action->setText(i18n("Open File Manager"));
    action->setIcon(KIcon(QStringLiteral("system-file-manager")));

    // Copy and Paste
    action = KStandardAction::copy(this, SLOT(copy()), collection);
    action->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_C));
    // disabled at first, since nothing has been selected now
    action->setEnabled(false);

    action = KStandardAction::paste(this, SLOT(paste()), collection);
    KShortcut pasteShortcut = action->shortcut();
    pasteShortcut.setPrimary(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_V));
    pasteShortcut.setAlternate(QKeySequence(Qt::SHIFT + Qt::Key_Insert));
    action->setShortcut(pasteShortcut);

    action = collection->addAction(QStringLiteral("paste-selection"), this, SLOT(pasteFromX11Selection()));
    action->setText(i18n("Paste Selection"));
    action->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_Insert));

    _webSearchMenu = new KActionMenu(i18n("Web Search"), this);
    _webSearchMenu->setIcon(KIcon(QStringLiteral("preferences-web-browser-shortcuts")));
    _webSearchMenu->setVisible(false);
    collection->addAction(QStringLiteral("web-search"), _webSearchMenu);


    action = collection->addAction(QStringLiteral("select-all"), this, SLOT(selectAll()));
    action->setText(i18n("&Select All"));
    action->setIcon(KIcon(QStringLiteral("edit-select-all")));

    action = collection->addAction(QStringLiteral("select-line"), this, SLOT(selectLine()));
    action->setText(i18n("Select &Line"));

    action = KStandardAction::saveAs(this, SLOT(saveHistory()), collection);
    action->setText(i18n("Save Output &As..."));

    action = KStandardAction::print(this, SLOT(print_screen()), collection);
    action->setText(i18n("&Print Screen..."));
    action->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_P));

    action = collection->addAction(QStringLiteral("adjust-history"), this, SLOT(showHistoryOptions()));
    action->setText(i18n("Adjust Scrollback..."));
    action->setIcon(KIcon(QStringLiteral("configure")));

    action = collection->addAction(QStringLiteral("clear-history"), this, SLOT(clearHistory()));
    action->setText(i18n("Clear Scrollback"));
    action->setIcon(KIcon(QStringLiteral("edit-clear-history")));

    action = collection->addAction(QStringLiteral("clear-history-and-reset"), this, SLOT(clearHistoryAndReset()));
    action->setText(i18n("Clear Scrollback and Reset"));
    action->setIcon(KIcon(QStringLiteral("edit-clear-history")));
    action->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_K));

    // Profile Options
    action = collection->addAction(QStringLiteral("edit-current-profile"), this, SLOT(editCurrentProfile()));
    action->setText(i18n("Edit Current Profile..."));
    action->setIcon(KIcon(QStringLiteral("document-properties")));

    _switchProfileMenu = new KActionMenu(i18n("Switch Profile"), this);
    collection->addAction(QStringLiteral("switch-profile"), _switchProfileMenu);
    connect(_switchProfileMenu->menu(), &QMenu::aboutToShow, this, &Konsole::SessionController::prepareSwitchProfileMenu);

    // History
    _findAction = KStandardAction::find(this, SLOT(searchBarEvent()), collection);
    _findAction->setShortcut(QKeySequence());

    _findNextAction = KStandardAction::findNext(this, SLOT(findNextInHistory()), collection);
    _findNextAction->setShortcut(QKeySequence());
    _findNextAction->setEnabled(false);

    _findPreviousAction = KStandardAction::findPrev(this, SLOT(findPreviousInHistory()), collection);
    _findPreviousAction->setShortcut(QKeySequence());
    _findPreviousAction->setEnabled(false);

    // Character Encoding
    _codecAction = new KCodecAction(i18n("Set &Encoding"), this);
    _codecAction->setIcon(KIcon(QStringLiteral("character-set")));
    collection->addAction(QStringLiteral("set-encoding"), _codecAction);
    connect(_codecAction->menu(), &QMenu::aboutToShow, this, &Konsole::SessionController::updateCodecAction);
    connect(_codecAction, static_cast<void(KCodecAction::*)(QTextCodec*)>(&KCodecAction::triggered), this, &Konsole::SessionController::changeCodec);
}

void SessionController::setupExtraActions()
{
    KActionCollection* collection = actionCollection();

    // Rename Session
    KAction* action = collection->addAction(QStringLiteral("rename-session"), this, SLOT(renameSession()));
    action->setText(i18n("&Rename Tab..."));
    action->setIcon(KIcon(QStringLiteral("edit-rename")));
    action->setShortcut(QKeySequence(Qt::CTRL + Qt::ALT + Qt::Key_S));

    // Copy input to ==> all tabs
    KToggleAction* copyInputToAllTabsAction = collection->add<KToggleAction>(QStringLiteral("copy-input-to-all-tabs"));
    copyInputToAllTabsAction->setText(i18n("&All Tabs in Current Window"));
    copyInputToAllTabsAction->setData(CopyInputToAllTabsMode);
    // this action is also used in other place, so remember it
    _copyInputToAllTabsAction = copyInputToAllTabsAction;

    // Copy input to ==> selected tabs
    KToggleAction* copyInputToSelectedTabsAction = collection->add<KToggleAction>(QStringLiteral("copy-input-to-selected-tabs"));
    copyInputToSelectedTabsAction->setText(i18n("&Select Tabs..."));
    copyInputToSelectedTabsAction->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_Period));
    copyInputToSelectedTabsAction->setData(CopyInputToSelectedTabsMode);

    // Copy input to ==> none
    KToggleAction* copyInputToNoneAction = collection->add<KToggleAction>(QStringLiteral("copy-input-to-none"));
    copyInputToNoneAction->setText(i18nc("@action:inmenu Do not select any tabs", "&None"));
    copyInputToNoneAction->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_Slash));
    copyInputToNoneAction->setData(CopyInputToNoneMode);
    copyInputToNoneAction->setChecked(true); // the default state

    // The "Copy Input To" submenu
    // The above three choices are represented as combo boxes
    KSelectAction* copyInputActions = collection->add<KSelectAction>(QStringLiteral("copy-input-to"));
    copyInputActions->setText(i18n("Copy Input To"));
    copyInputActions->addAction(copyInputToAllTabsAction);
    copyInputActions->addAction(copyInputToSelectedTabsAction);
    copyInputActions->addAction(copyInputToNoneAction);
    connect(copyInputActions, static_cast<void(KSelectAction::*)(QAction*)>(&KSelectAction::triggered), this, &Konsole::SessionController::copyInputActionsTriggered);

    action = collection->addAction(QStringLiteral("zmodem-upload"), this, SLOT(zmodemUpload()));
    action->setText(i18n("&ZModem Upload..."));
    action->setIcon(KIcon(QStringLiteral("document-open")));
    action->setShortcut(QKeySequence(Qt::CTRL + Qt::ALT + Qt::Key_U));

    // Monitor
    KToggleAction* toggleAction = new KToggleAction(i18n("Monitor for &Activity"), this);
    toggleAction->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_A));
    action = collection->addAction(QStringLiteral("monitor-activity"), toggleAction);
    connect(action, &QAction::toggled, this, &Konsole::SessionController::monitorActivity);

    toggleAction = new KToggleAction(i18n("Monitor for &Silence"), this);
    toggleAction->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_I));
    action = collection->addAction(QStringLiteral("monitor-silence"), toggleAction);
    connect(action, &QAction::toggled, this, &Konsole::SessionController::monitorSilence);

    toggleAction = new KToggleAction(i18n("Monitor for Process Finishing"), this);
    action = collection->addAction(QStringLiteral("monitor-process-finish"), toggleAction);
    connect(action, &QAction::toggled, this, &Konsole::SessionController::monitorProcessFinish);


    // Text Size
    action = collection->addAction(QStringLiteral("enlarge-font"), this, SLOT(increaseFontSize()));
    action->setText(i18n("Enlarge Font"));
    action->setIcon(KIcon(QStringLiteral("format-font-size-more")));
    KShortcut enlargeFontShortcut = action->shortcut();
    enlargeFontShortcut.setPrimary(QKeySequence(Qt::CTRL + Qt::Key_Plus));
    enlargeFontShortcut.setAlternate(QKeySequence(Qt::CTRL + Qt::Key_Equal));
    action->setShortcut(enlargeFontShortcut);

    action = collection->addAction(QStringLiteral("shrink-font"), this, SLOT(decreaseFontSize()));
    action->setText(i18n("Shrink Font"));
    action->setIcon(KIcon(QStringLiteral("format-font-size-less")));
    action->setShortcut(KShortcut(Qt::CTRL | Qt::Key_Minus));

    action = collection->addAction(QStringLiteral("reset-font-size"), this, SLOT(resetFontSize()));
    action->setText(i18n("Reset Font Size"));
    action->setShortcut(KShortcut(Qt::ALT + Qt::Key_0));

    // Send signal
    KSelectAction* sendSignalActions = collection->add<KSelectAction>(QStringLiteral("send-signal"));
    sendSignalActions->setText(i18n("Send Signal"));
    connect(sendSignalActions, static_cast<void(KSelectAction::*)(QAction*)>(&KSelectAction::triggered), this, &Konsole::SessionController::sendSignal);

    action = collection->addAction(QStringLiteral("sigstop-signal"));
    action->setText(i18n("&Suspend Task")   + QStringLiteral(" (STOP)"));
    action->setData(SIGSTOP);
    sendSignalActions->addAction(action);

    action = collection->addAction(QStringLiteral("sigcont-signal"));
    action->setText(i18n("&Continue Task")  + QStringLiteral(" (CONT)"));
    action->setData(SIGCONT);
    sendSignalActions->addAction(action);

    action = collection->addAction(QStringLiteral("sighup-signal"));
    action->setText(i18n("&Hangup")         + QStringLiteral(" (HUP)"));
    action->setData(SIGHUP);
    sendSignalActions->addAction(action);

    action = collection->addAction(QStringLiteral("sigint-signal"));
    action->setText(i18n("&Interrupt Task") + QStringLiteral(" (INT)"));
    action->setData(SIGINT);
    sendSignalActions->addAction(action);

    action = collection->addAction(QStringLiteral("sigterm-signal"));
    action->setText(i18n("&Terminate Task") + QStringLiteral(" (TERM)"));
    action->setData(SIGTERM);
    sendSignalActions->addAction(action);

    action = collection->addAction(QStringLiteral("sigkill-signal"));
    action->setText(i18n("&Kill Task")      + QStringLiteral(" (KILL)"));
    action->setData(SIGKILL);
    sendSignalActions->addAction(action);

    action = collection->addAction(QStringLiteral("sigusr1-signal"));
    action->setText(i18n("User Signal &1")   + QStringLiteral(" (USR1)"));
    action->setData(SIGUSR1);
    sendSignalActions->addAction(action);

    action = collection->addAction(QStringLiteral("sigusr2-signal"));
    action->setText(i18n("User Signal &2")   + QStringLiteral(" (USR2)"));
    action->setData(SIGUSR2);
    sendSignalActions->addAction(action);

    _findAction->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_F));
    _findNextAction->setShortcut(QKeySequence(Qt::Key_F3));
    _findPreviousAction->setShortcut(QKeySequence(Qt::SHIFT + Qt::Key_F3));
}

void SessionController::switchProfile(Profile::Ptr profile)
{
    SessionManager::instance()->setSessionProfile(_sessionDisplayConnection->session(), profile);
}

void SessionController::prepareSwitchProfileMenu()
{
    if (_switchProfileMenu->menu()->isEmpty()) {
        _profileList = new ProfileList(false, this);
        connect(_profileList, &Konsole::ProfileList::profileSelected, this, &Konsole::SessionController::switchProfile);
    }

    _switchProfileMenu->menu()->clear();
    _switchProfileMenu->menu()->addActions(_profileList->actions());
}
void SessionController::updateCodecAction()
{
    _codecAction->setCurrentCodec(QString::fromUtf8(_sessionDisplayConnection->session()->codec()));
}

void SessionController::changeCodec(QTextCodec* codec)
{
    _sessionDisplayConnection->session()->setCodec(codec);
}

EditProfileDialog* SessionController::profileDialogPointer()
{
    return _editProfileDialog.data();
}

void SessionController::editCurrentProfile()
{
    // Searching for Edit profile dialog opened with the same profile
    for (SessionController *controller: std::as_const(_allControllers)) {
        if (controller->profileDialogPointer()
                && controller->profileDialogPointer()->isVisible()
                && controller->profileDialogPointer()->lookupProfile()
                    == SessionManager::instance()->sessionProfile(_sessionDisplayConnection->session())) {
            controller->profileDialogPointer()->close();
        }
    }

    // NOTE bug311270: For to prevent the crash, the profile must be reset.
    if (!_editProfileDialog.isNull()) {
        // exists but not visible
        _editProfileDialog->deleteLater();
    }

    _editProfileDialog = new EditProfileDialog(QApplication::activeWindow());
    _editProfileDialog->setProfile(SessionManager::instance()->sessionProfile(_sessionDisplayConnection->session()));
    _editProfileDialog->show();
}

void SessionController::renameSession()
{
    const QString &sessionLocalTabTitleFormat = _sessionDisplayConnection->session()->tabTitleFormat(Session::LocalTabTitle);
    const QString &sessionRemoteTabTitleFormat = _sessionDisplayConnection->session()->tabTitleFormat(Session::RemoteTabTitle);

    QScopedPointer<RenameTabDialog> dialog(new RenameTabDialog(QApplication::activeWindow()));
    dialog->setTabTitleText(sessionLocalTabTitleFormat);
    dialog->setRemoteTabTitleText(sessionRemoteTabTitleFormat);

    if (_sessionDisplayConnection->session()->isRemote()) {
        dialog->focusRemoteTabTitleText();
    } else {
        dialog->focusTabTitleText();
    }

    QPointer<Session> guard(_sessionDisplayConnection->session());
    int result = dialog->exec();
    if (guard.isNull())
        return;

    if (result != 0) {
        const QString &tabTitle = dialog->tabTitleText();
        const QString &remoteTabTitle = dialog->remoteTabTitleText();

        if (tabTitle != sessionLocalTabTitleFormat) {
            _sessionDisplayConnection->session()->setTabTitleFormat(Session::LocalTabTitle, tabTitle);
            emit tabRenamedByUser(true);
            // trigger an update of the tab text
            snapshot();
        }

        if(remoteTabTitle != sessionRemoteTabTitleFormat) {
            _sessionDisplayConnection->session()->setTabTitleFormat(Session::RemoteTabTitle, remoteTabTitle);
            emit tabRenamedByUser(true);
            snapshot();
        }
    }
}

bool SessionController::confirmClose() const
{
    if (_sessionDisplayConnection->session()->isForegroundProcessActive()) {
        QString title = _sessionDisplayConnection->session()->foregroundProcessName();

        // hard coded for now.  In future make it possible for the user to specify which programs
        // are ignored when considering whether to display a confirmation
        QStringList ignoreList;
        ignoreList << QString::fromUtf8(qgetenv("SHELL")).section(QLatin1Char('/'), -1);
        if (ignoreList.contains(title))
            return true;

        QString question;
        if (title.isEmpty())
            question = i18n("A program is currently running in this session."
                            "  Are you sure you want to close it?");
        else
            question = i18n("The program '%1' is currently running in this session."
                            "  Are you sure you want to close it?", title);

        int result = KMessageBox::warningYesNo(_sessionDisplayConnection->view()->window(), question, i18n("Confirm Close"));
        return result == KMessageBox::Yes;
    }
    return true;
}
bool SessionController::confirmForceClose() const
{
    if (_sessionDisplayConnection->session()->isRunning()) {
        QString title = _sessionDisplayConnection->session()->program();

        // hard coded for now.  In future make it possible for the user to specify which programs
        // are ignored when considering whether to display a confirmation
        QStringList ignoreList;
        ignoreList << QString::fromUtf8(qgetenv("SHELL")).section(QLatin1Char('/'), -1);
        if (ignoreList.contains(title))
            return true;

        QString question;
        if (title.isEmpty())
            question = i18n("A program in this session would not die."
                            "  Are you sure you want to kill it by force?");
        else
            question = i18n("The program '%1' is in this session would not die."
                            "  Are you sure you want to kill it by force?", title);

        int result = KMessageBox::warningYesNo(_sessionDisplayConnection->view()->window(), question, i18n("Confirm Close"));
        return result == KMessageBox::Yes;
    }
    return true;
}
void SessionController::closeSession()
{
    if (_preventClose)
        return;

    if (!confirmClose())
        return;

    if (_sessionDisplayConnection->session()->closeInNormalWay()) {
        if (!confirmForceClose())
            return;

        if (!_sessionDisplayConnection->session()->closeInForceWay()) {
            kWarning() << "Konsole failed to close a session in any way.";
            return;
        }
    }

    if (factory())
        factory()->removeClient(this);
}

// Trying to open a remote Url may produce unexpected results.
// Therefore, if a remote url, open the user's home path.
// TODO consider: 1) disable menu upon remote session
//   2) transform url to get the desired result (ssh -> sftp, etc)
void SessionController::openBrowser()
{
    KUrl currentUrl = url();

    if (currentUrl.isLocalFile())
        new KRun(currentUrl, QApplication::activeWindow(), 0, true, true);
    else
        new KRun(KUrl(QDir::homePath()), QApplication::activeWindow(), 0, true, true);
}

void SessionController::copy()
{
    _sessionDisplayConnection->view()->copyToClipboard();
}

void SessionController::paste()
{
    _sessionDisplayConnection->view()->pasteFromClipboard();
}
void SessionController::pasteFromX11Selection()
{
    _sessionDisplayConnection->view()->pasteFromX11Selection();
}
void SessionController::selectAll()
{
    _sessionDisplayConnection->view()->selectAll();
}
void SessionController::selectLine()
{
    _sessionDisplayConnection->view()->selectCurrentLine();
}
static const KXmlGuiWindow* findWindow(const QObject* object)
{
    // Walk up the QObject hierarchy to find a KXmlGuiWindow.
    while (object != nullptr) {
        const KXmlGuiWindow* window = qobject_cast<const KXmlGuiWindow*>(object);
        if (window != nullptr) {
            return(window);
        }
        object = object->parent();
    }
    return(nullptr);
}

static bool hasTerminalDisplayInSameWindow(const Session* session, const KXmlGuiWindow* window)
{
    // Iterate all TerminalDisplays of this Session ...
    const QList<TerminalDisplay *> views = session->views();
    for (const TerminalDisplay *terminalDisplay: views) {
        // ... and check whether a TerminalDisplay has the same
        // window as given in the parameter
        if (window == findWindow(terminalDisplay)) {
            return(true);
        }
    }
    return(false);
}

void SessionController::copyInputActionsTriggered(QAction* action)
{
    const int mode = action->data().toInt();

    switch (mode) {
    case CopyInputToAllTabsMode:
        copyInputToAllTabs();
        break;
    case CopyInputToSelectedTabsMode:
        copyInputToSelectedTabs();
        break;
    case CopyInputToNoneMode:
        copyInputToNone();
        break;
    default:
        Q_ASSERT(false);
    }
}

void SessionController::copyInputToAllTabs()
{
    if (!_copyToGroup) {
        _copyToGroup = new SessionGroup(this);
    }

    // Find our window ...
    const KXmlGuiWindow* myWindow = findWindow(_sessionDisplayConnection->view());

    QSet<Session*> group =
        QSet<Session*>::fromList(SessionManager::instance()->sessions());
    for (auto session : group) {

        // First, ensure that the session is removed
        // (necessary to avoid duplicates on addSession()!)
        _copyToGroup->removeSession(session);

        // Add current session if it is displayed our window
        if (hasTerminalDisplayInSameWindow(session, myWindow)) {
            _copyToGroup->addSession(session);
        }
    }
    _copyToGroup->setMasterStatus(_sessionDisplayConnection->session(), true);
    _copyToGroup->setMasterMode(SessionGroup::CopyInputToAll);

    snapshot();
}

void SessionController::copyInputToSelectedTabs()
{
    if (!_copyToGroup) {
        _copyToGroup = new SessionGroup(this);
        _copyToGroup->addSession(_sessionDisplayConnection->session());
        _copyToGroup->setMasterStatus(_sessionDisplayConnection->session(), true);
        _copyToGroup->setMasterMode(SessionGroup::CopyInputToAll);
    }

    QPointer<CopyInputDialog> dialog = new CopyInputDialog(_sessionDisplayConnection->view());
    dialog->setMasterSession(_sessionDisplayConnection->session());

    QSet<Session*> currentGroup = QSet<Session*>::fromList(_copyToGroup->sessions());
    currentGroup.remove(_sessionDisplayConnection->session());

    dialog->setChosenSessions(currentGroup);

    QPointer<Session> guard(_sessionDisplayConnection->session());
    int result = dialog->exec();
    if (guard.isNull())
        return;

    if (result == QDialog::Accepted) {
        QSet<Session*> newGroup = dialog->chosenSessions();
        newGroup.remove(_sessionDisplayConnection->session());

        const QSet<Session *> completeGroup = newGroup | currentGroup;
        for (Session *session: completeGroup) {
            if (newGroup.contains(session) && !currentGroup.contains(session))
                _copyToGroup->addSession(session);
            else if (!newGroup.contains(session) && currentGroup.contains(session))
                _copyToGroup->removeSession(session);
        }

        _copyToGroup->setMasterStatus(_sessionDisplayConnection->session(), true);
        _copyToGroup->setMasterMode(SessionGroup::CopyInputToAll);
        snapshot();
    }
}

void SessionController::copyInputToNone()
{
    if (!_copyToGroup)      // No 'Copy To' is active
        return;

    QSet<Session*> group =
        QSet<Session*>::fromList(SessionManager::instance()->sessions());
    for (auto iterator : group) {
        Session* session = iterator;

        if (session != _sessionDisplayConnection->session()) {
            _copyToGroup->removeSession(iterator);
        }
    }
    delete _copyToGroup;
    _copyToGroup = nullptr;
    snapshot();
}

void SessionController::searchClosed()
{
    _isSearchBarEnabled = false;
    searchHistory(false);
}

void SessionController::setSearchStartToWindowCurrentLine()
{
    setSearchStartTo(-1);
}

void SessionController::setSearchStartTo(int line)
{
    _searchStartLine = line;
    _prevSearchResultLine = line;
}

void SessionController::listenForScreenWindowUpdates()
{
    if (_listenForScreenWindowUpdates)
        return;

    connect(_sessionDisplayConnection->view()->screenWindow(), &Konsole::ScreenWindow::outputChanged, this,
            &Konsole::SessionController::updateSearchFilter);
    connect(_sessionDisplayConnection->view()->screenWindow(), &Konsole::ScreenWindow::scrolled, this,
            &Konsole::SessionController::updateSearchFilter);
    connect(_sessionDisplayConnection->view()->screenWindow(), &Konsole::ScreenWindow::currentResultLineChanged, _sessionDisplayConnection->view(),
            static_cast<void(TerminalDisplay::*)()>(&Konsole::TerminalDisplay::update));

    _listenForScreenWindowUpdates = true;
}

void SessionController::updateSearchFilter()
{
    if (_searchFilter && !_searchBar.isNull()) {
        _sessionDisplayConnection->view()->processFilters();
    }
}

void SessionController::searchBarEvent()
{
    QString selectedText = _sessionDisplayConnection->view()->screenWindow()->selectedText(Screen::PreserveLineBreaks | Screen::TrimLeadingWhitespace | Screen::TrimTrailingWhitespace);
    if (!selectedText.isEmpty())
        _searchBar->setSearchText(selectedText);

    if (_searchBar->isVisible()) {
        _searchBar->focusLineEdit();
    } else {
        searchHistory(true);
        _isSearchBarEnabled = true;
    }
}

void SessionController::enableSearchBar(bool showSearchBar)
{
    if (_searchBar.isNull())
        return;

    if (showSearchBar && !_searchBar->isVisible()) {
        setSearchStartToWindowCurrentLine();
    }

    _searchBar->setVisible(showSearchBar);
    if (showSearchBar) {
        connect(_searchBar, &Konsole::IncrementalSearchBar::searchChanged, this,
                &Konsole::SessionController::searchTextChanged);
        connect(_searchBar, &Konsole::IncrementalSearchBar::searchReturnPressed, this,
                &Konsole::SessionController::findPreviousInHistory);
        connect(_searchBar, &Konsole::IncrementalSearchBar::searchShiftPlusReturnPressed, this,
                &Konsole::SessionController::findNextInHistory);
    } else {
        disconnect(_searchBar, &Konsole::IncrementalSearchBar::searchChanged, this,
                   &Konsole::SessionController::searchTextChanged);
        disconnect(_searchBar, &Konsole::IncrementalSearchBar::searchReturnPressed, this,
                   &Konsole::SessionController::findPreviousInHistory);
        disconnect(_searchBar, &Konsole::IncrementalSearchBar::searchShiftPlusReturnPressed, this,
                   &Konsole::SessionController::findNextInHistory);
        if ((!_sessionDisplayConnection->view().isNull()) && _sessionDisplayConnection->view()->screenWindow()) {
            _sessionDisplayConnection->view()->screenWindow()->setCurrentResultLine(-1);
        }
    }
}


bool SessionController::reverseSearchChecked() const
{
    Q_ASSERT(_searchBar);

    QBitArray options = _searchBar->optionsChecked();
    return options.at(IncrementalSearchBar::ReverseSearch);
}

QRegExp SessionController::regexpFromSearchBarOptions()
{
    QBitArray options = _searchBar->optionsChecked();

    Qt::CaseSensitivity caseHandling = options.at(IncrementalSearchBar::MatchCase) ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QRegExp::PatternSyntax syntax = options.at(IncrementalSearchBar::RegExp) ? QRegExp::RegExp : QRegExp::FixedString;

    QRegExp regExp(_searchBar->searchText(),  caseHandling , syntax);
    return regExp;
}

// searchHistory() may be called either as a result of clicking a menu item or
// as a result of changing the search bar widget
void SessionController::searchHistory(bool showSearchBar)
{
    enableSearchBar(showSearchBar);

    if (!_searchBar.isNull()) {
        if (showSearchBar) {
            removeSearchFilter();

            listenForScreenWindowUpdates();

            _searchFilter = new RegExpFilter();
            _searchFilter->setRegExp(regexpFromSearchBarOptions());
            _sessionDisplayConnection->view()->filterChain()->addFilter(_searchFilter);
            _sessionDisplayConnection->view()->processFilters();

            setFindNextPrevEnabled(true);
        } else {
            setFindNextPrevEnabled(false);

            removeSearchFilter();

            _sessionDisplayConnection->view()->setFocus(Qt::ActiveWindowFocusReason);
        }
    }
}

void SessionController::setFindNextPrevEnabled(bool enabled)
{
    _findNextAction->setEnabled(enabled);
    _findPreviousAction->setEnabled(enabled);
}
void SessionController::searchTextChanged(const QString& text)
{
    Q_ASSERT(_sessionDisplayConnection->view()->screenWindow());

    if (_searchText == text)
        return;

    _searchText = text;

    if (text.isEmpty()) {
        _sessionDisplayConnection->view()->screenWindow()->clearSelection();
        _sessionDisplayConnection->view()->screenWindow()->scrollTo(_searchStartLine);
    }

    // update search.  this is called even when the text is
    // empty to clear the view's filters
    beginSearch(text , reverseSearchChecked() ? Enum::BackwardsSearch : Enum::ForwardsSearch);
}
void SessionController::searchCompleted(bool success)
{
    _prevSearchResultLine = _sessionDisplayConnection->view()->screenWindow()->currentResultLine();

    if (!_searchBar.isNull())
        _searchBar->setFoundMatch(success);
}

void SessionController::beginSearch(const QString& text, Enum::SearchDirection direction)
{
    Q_ASSERT(_searchBar);
    Q_ASSERT(_searchFilter);

    QRegExp regExp = regexpFromSearchBarOptions();
    _searchFilter->setRegExp(regExp);

    if (_searchStartLine < 0 || _searchStartLine > _sessionDisplayConnection->view()->screenWindow()->lineCount()) {
        if (direction == Enum::ForwardsSearch) {
            setSearchStartTo(_sessionDisplayConnection->view()->screenWindow()->currentLine());
        } else {
            setSearchStartTo(_sessionDisplayConnection->view()->screenWindow()->currentLine() + _sessionDisplayConnection->view()->screenWindow()->windowLines());
        }
    }

    if (!regExp.isEmpty()) {
        _sessionDisplayConnection->view()->screenWindow()->setCurrentResultLine(-1);
        SearchHistoryTask* task = new SearchHistoryTask(this);

        connect(task, &Konsole::SearchHistoryTask::completed, this, &Konsole::SessionController::searchCompleted);

        task->setRegExp(regExp);
        task->setSearchDirection(direction);
        task->setAutoDelete(true);
        task->setStartLine(_searchStartLine);
        task->addScreenWindow(_sessionDisplayConnection->session(), _sessionDisplayConnection->view()->screenWindow());
        task->execute();
    } else if (text.isEmpty()) {
        searchCompleted(false);
    }

    _sessionDisplayConnection->view()->processFilters();
}
void SessionController::highlightMatches(bool highlight)
{
    if (highlight) {
        _sessionDisplayConnection->view()->filterChain()->addFilter(_searchFilter);
        _sessionDisplayConnection->view()->processFilters();
    } else {
        _sessionDisplayConnection->view()->filterChain()->removeFilter(_searchFilter);
    }

    _sessionDisplayConnection->view()->update();
}

void SessionController::searchFrom()
{
    Q_ASSERT(_searchBar);
    Q_ASSERT(_searchFilter);

    if (reverseSearchChecked()) {
        setSearchStartTo(_sessionDisplayConnection->view()->screenWindow()->lineCount());
    } else {
        setSearchStartTo(0);
    }


    beginSearch(_searchBar->searchText(), reverseSearchChecked() ? Enum::BackwardsSearch : Enum::ForwardsSearch);
}
void SessionController::findNextInHistory()
{
    Q_ASSERT(_searchBar);
    Q_ASSERT(_searchFilter);

    setSearchStartTo(_prevSearchResultLine);

    beginSearch(_searchBar->searchText(), reverseSearchChecked() ? Enum::BackwardsSearch : Enum::ForwardsSearch);
}
void SessionController::findPreviousInHistory()
{
    Q_ASSERT(_searchBar);
    Q_ASSERT(_searchFilter);

    setSearchStartTo(_prevSearchResultLine);

    beginSearch(_searchBar->searchText(), reverseSearchChecked() ? Enum::ForwardsSearch : Enum::BackwardsSearch);
}
void SessionController::changeSearchMatch()
{
    Q_ASSERT(_searchBar);
    Q_ASSERT(_searchFilter);

    // reset Selection for new case match
    _sessionDisplayConnection->view()->screenWindow()->clearSelection();
    beginSearch(_searchBar->searchText(), reverseSearchChecked() ? Enum::BackwardsSearch : Enum::ForwardsSearch);
}
void SessionController::showHistoryOptions()
{
    QScopedPointer<HistorySizeDialog> dialog(new HistorySizeDialog(QApplication::activeWindow()));
    const HistoryType& currentHistory = _sessionDisplayConnection->session()->historyType();

    if (currentHistory.isEnabled()) {
        if (currentHistory.isUnlimited()) {
            dialog->setMode(Enum::UnlimitedHistory);
        } else {
            dialog->setMode(Enum::FixedSizeHistory);
            dialog->setLineCount(currentHistory.maximumLineCount());
        }
    } else {
        dialog->setMode(Enum::NoHistory);
    }

    QPointer<Session> guard(_sessionDisplayConnection->session());
    int result = dialog->exec();
    if (guard.isNull())
        return;

    if (result != 0) {
        scrollBackOptionsChanged(dialog->mode(), dialog->lineCount());
    }
}
void SessionController::sessionResizeRequest(const QSize& size)
{
    //kDebug() << "View resize requested to " << size;
    _sessionDisplayConnection->view()->setSize(size.width(), size.height());
}
void SessionController::scrollBackOptionsChanged(int mode, int lines)
{
    switch (mode) {
    case Enum::NoHistory:
        _sessionDisplayConnection->session()->setHistoryType(HistoryTypeNone());
        break;
    case Enum::FixedSizeHistory:
        _sessionDisplayConnection->session()->setHistoryType(CompactHistoryType(lines));
        break;
    case Enum::UnlimitedHistory:
        _sessionDisplayConnection->session()->setHistoryType(HistoryTypeFile());
        break;
    }
}

void SessionController::print_screen()
{
    QPrinter printer;

    QPointer<QPrintDialog> dialog = new QPrintDialog(&printer, _sessionDisplayConnection->view());
    PrintOptions* options = new PrintOptions();

    dialog->setOptionTabs({options});
    dialog->setWindowTitle(i18n("Print Shell"));
    connect(dialog, static_cast<void(QPrintDialog::*)()>(&QPrintDialog::accepted), options, &Konsole::PrintOptions::saveSettings);
    if (dialog->exec() != QDialog::Accepted)
        return;

    QPainter painter;
    painter.begin(&printer);

    KConfigGroup configGroup(KGlobal::config(), "PrintOptions");

    if (configGroup.readEntry("ScaleOutput", true)) {
        double scale = std::min(printer.pageRect().width() / static_cast<double>(_sessionDisplayConnection->view()->width()),
                            printer.pageRect().height() / static_cast<double>(_sessionDisplayConnection->view()->height()));
        painter.scale(scale, scale);
    }

    _sessionDisplayConnection->view()->printContent(painter, configGroup.readEntry("PrinterFriendly", true));
}

void SessionController::saveHistory()
{
    SessionTask* task = new SaveHistoryTask(this);
    task->setAutoDelete(true);
    task->addSession(_sessionDisplayConnection->session());
    task->execute();
}

void SessionController::clearHistory()
{
    _sessionDisplayConnection->session()->clearHistory();
    _sessionDisplayConnection->view()->updateImage();   // To reset view scrollbar
    _sessionDisplayConnection->view()->repaint();
}

void SessionController::clearHistoryAndReset()
{
    Profile::Ptr profile = SessionManager::instance()->sessionProfile(_sessionDisplayConnection->session());
    QByteArray name = profile->defaultEncoding().toUtf8();

    Emulation* emulation = _sessionDisplayConnection->session()->emulation();
    emulation->reset();
    _sessionDisplayConnection->session()->refresh();
    _sessionDisplayConnection->session()->setCodec(QTextCodec::codecForName(name));
    clearHistory();
}

void SessionController::increaseFontSize()
{
    _sessionDisplayConnection->view()->increaseFontSize();
}

void SessionController::decreaseFontSize()
{
    _sessionDisplayConnection->view()->decreaseFontSize();
}

void SessionController::resetFontSize()
{
    _sessionDisplayConnection->view()->resetFontSize();
}

void SessionController::monitorActivity(bool monitor)
{
    _sessionDisplayConnection->session()->setMonitorActivity(monitor);
}
void SessionController::monitorSilence(bool monitor)
{
    _sessionDisplayConnection->session()->setMonitorSilence(monitor);
}
void SessionController::monitorProcessFinish(bool monitor)
{
    _monitorProcessFinish = monitor;
}
void SessionController::updateSessionIcon()
{
    // Visualize that the session is broadcasting to others
    if (_copyToGroup && _copyToGroup->sessions().count() > 1) {
        // Master Mode: set different icon, to warn the user to be careful
        setIcon(_broadcastIcon);
    } else {
        if (!_keepIconUntilInteraction) {
            // Not in Master Mode: use normal icon
            setIcon(_sessionIcon);
        }
    }
}
void SessionController::sessionAttributeChanged()
{
    if (_sessionIconName != _sessionDisplayConnection->session()->iconName()) {
        _sessionIconName = _sessionDisplayConnection->session()->iconName();
        _sessionIcon = KIcon(_sessionIconName);
        updateSessionIcon();
    }

    QString title = _sessionDisplayConnection->session()->title(Session::DisplayedTitleRole);

    // special handling for the "%w" marker which is replaced with the
    // window title set by the shell
    title.replace(QLatin1String("%w"), _sessionDisplayConnection->session()->userTitle());
    // special handling for the "%#" marker which is replaced with the
    // number of the shell
    title.replace(QLatin1String("%#"), QString::number(_sessionDisplayConnection->session()->sessionId()));

    if (title.isEmpty())
        title = _sessionDisplayConnection->session()->title(Session::NameRole);

    setTitle(title);
    emit rawTitleChanged();
}

void SessionController::showDisplayContextMenu(const QPoint& position)
{
    // needed to make sure the popup menu is available, even if a hosting
    // application did not merge our GUI.
    if (!factory()) {
        if (!clientBuilder()) {
            setClientBuilder(new KXMLGUIBuilder(_sessionDisplayConnection->view()));

            // Client builder does not get deleted automatically
            connect(this, &QObject::destroyed, this, [this]{ delete clientBuilder(); });
        }

        KXMLGUIFactory* factory = new KXMLGUIFactory(clientBuilder(), _sessionDisplayConnection->view());
        factory->addClient(this);
    }

    QPointer<QMenu> popup = qobject_cast<QMenu*>(factory()->container(QStringLiteral("session-popup-menu"), this));
    if (!popup.isNull()) {
        // prepend content-specific actions such as "Open Link", "Copy Email Address" etc.
        QList<QAction*> contentActions = _sessionDisplayConnection->view()->filterActions(position);
        QAction* contentSeparator = new QAction(popup);
        contentSeparator->setSeparator(true);
        contentActions << contentSeparator;
        popup->insertActions(popup->actions().value(0, nullptr), contentActions);

        // always update this submenu before showing the context menu,
        // because the available search services might have changed
        // since the context menu is shown last time
        updateWebSearchMenu();

        _preventClose = true;

        if (_showMenuAction) {
            if (  _showMenuAction->isChecked() ) {
                popup->removeAction( _showMenuAction);
            } else {
                popup->insertAction(_switchProfileMenu, _showMenuAction);
            }
        }

        QAction* chosen = popup->exec(_sessionDisplayConnection->view()->mapToGlobal(position));

        // check for validity of the pointer to the popup menu
        if (!popup.isNull()) {
            delete contentSeparator;
        }

        _preventClose = false;

        if (chosen && chosen->objectName() == QLatin1String("close-session"))
            chosen->trigger();
    } else {
        kWarning() << "Unable to display popup menu for session"
                   << _sessionDisplayConnection->session()->title(Session::NameRole)
                   << ", no GUI factory available to build the popup.";
    }
}

void SessionController::movementKeyFromSearchBarReceived(QKeyEvent *event)
{
    QCoreApplication::sendEvent(_sessionDisplayConnection->view(), event);
    setSearchStartToWindowCurrentLine();
}

void SessionController::sessionNotificationsChanged(Session::Notification notification, bool enabled)
{
    if (notification == Session::Notification::Activity && enabled) {
        setIcon(_activityIcon);
        _keepIconUntilInteraction = true;
    } else if (notification == Session::Notification::Silence && enabled) {
        setIcon(_silenceIcon);
        _keepIconUntilInteraction = true;
    } else if (notification == Session::Notification::Bell && enabled) {
        setIcon(_bellIcon);
        _keepIconUntilInteraction = true;
    } else {
        if (_sessionIconName != _sessionDisplayConnection->session()->iconName()) {
            _sessionIconName = _sessionDisplayConnection->session()->iconName();
            _sessionIcon = KIcon(_sessionIconName);
        }
        updateSessionIcon();
    }

    emit notificationChanged(this, notification, enabled);
}

void SessionController::zmodemDownload()
{
    QString zmodem = KStandardDirs::findExe(QStringLiteral("rz"));
    if (zmodem.isEmpty()) {
        zmodem = KStandardDirs::findExe(QStringLiteral("lrz"));
    }
    if (!zmodem.isEmpty()) {
        const QString path = KFileDialog::getExistingDirectory(
                                 QString(), _sessionDisplayConnection->view(),
                                 i18n("Save ZModem Download to..."));

        if (!path.isEmpty()) {
            _sessionDisplayConnection->session()->startZModem(zmodem, path, QStringList());
            return;
        }
    } else {
        KMessageBox::error(_sessionDisplayConnection->view(),
                           i18n("<p>A ZModem file transfer attempt has been detected, "
                                "but no suitable ZModem software was found on this system.</p>"
                                "<p>You may wish to install the 'rzsz' or 'lrzsz' package.</p>"));
    }
    _sessionDisplayConnection->session()->cancelZModem();
}

void SessionController::zmodemUpload()
{
    if (_sessionDisplayConnection->session()->isZModemBusy()) {
        KMessageBox::sorry(_sessionDisplayConnection->view(),
                           i18n("<p>The current session already has a ZModem file transfer in progress.</p>"));
        return;
    }

    QString zmodem = KStandardDirs::findExe(QStringLiteral("sz"));
    if (zmodem.isEmpty()) {
        zmodem = KStandardDirs::findExe(QStringLiteral("lsz"));
    }
    if (zmodem.isEmpty()) {
        KMessageBox::sorry(_sessionDisplayConnection->view(),
                           i18n("<p>No suitable ZModem software was found on this system.</p>"
                                "<p>You may wish to install the 'rzsz' or 'lrzsz' package.</p>"));
        return;
    }

    QStringList files = KFileDialog::getOpenFileNames(KUrl(), QString(), _sessionDisplayConnection->view(),
                        i18n("Select Files for ZModem Upload"));
    if (!files.isEmpty()) {
        _sessionDisplayConnection->session()->startZModem(zmodem, QString(), files);
    }
}

bool SessionController::isKonsolePart() const
{
    // Check to see if we are being called from Konsole or a KPart
    return !(QString(qApp->metaObject()->className()) == QLatin1String("Konsole::Application"));
}

QString SessionController::userTitle() const
{
    if (!_sessionDisplayConnection->session().isNull()) {
        return _sessionDisplayConnection->session()->userTitle();
    } else {
        return QString();
    }
}

bool SessionController::isValid() const
{
    return _sessionDisplayConnection->isValid();
}
#include "session/SessionController.moc"
