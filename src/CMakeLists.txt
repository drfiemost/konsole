# cmake-options : -WITH_LibKonq=ON or OFF; default is ON
set(FPHSA_NAME_MISMATCHED TRUE)
macro_optional_find_package(LibKonq)
macro_bool_to_01(LIBKONQ_FOUND HAVE_LIBKONQ)

macro_log_feature(LIBKONQ_FOUND "LibKonq"
  "Provides high-level file management functions"
  "https://projects.kde.org/projects/kde/applications/kde-baseapps"
  FALSE "" "Required for drag-n-drop menus")

if(HAVE_LIBKONQ)
  include_directories(${LIBKONQ_INCLUDE_DIR})
endif()

### Too many crashes/issues with detaching on MacOSX
IF(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  set(ENABLE_DETACHING 0)
else()
  set(ENABLE_DETACHING 1)
endif()

### Handle DragonFlyBSD here instead of using __DragonFly__
IF(${CMAKE_SYSTEM_NAME} MATCHES "DragonFly")
  set(HAVE_OS_DRAGONFLYBSD 1)
else()
  set(HAVE_OS_DRAGONFLYBSD 0)
endif()

include(CheckIncludeFiles)

configure_file(config-konsole.h.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/config-konsole.h)

### Tests
macro_optional_add_subdirectory(tests)

### Security concerns about sendText and runCommand dbus methods being public
option(REMOVE_SENDTEXT_RUNCOMMAND_DBUS_METHODS "Konsole: remove sendText and runCommand dbus methods" OFF)

### Konsole source files shared between embedded terminal and main application
# qdbuscpp2xml -m  Session.h -o org.kde.konsole.Session.xml
# qdbuscpp2xml -M -s ViewManager.h -o org.kde.konsole.Konsole.xml

# Generate dbus .xml files; do not store .xml in source folder
qt4_generate_dbus_interface(session/Session.h org.kde.konsole.Session.xml OPTIONS -m)
qt4_generate_dbus_interface(ViewManager.h org.kde.konsole.Window.xml OPTIONS -m)

qt4_add_dbus_adaptor(sessionadaptors_SRCS
                    ${CMAKE_CURRENT_BINARY_DIR}/org.kde.konsole.Session.xml
                    session/Session.h
                    Konsole::Session)
qt4_add_dbus_adaptor(windowadaptors_SRCS
                    ${CMAKE_CURRENT_BINARY_DIR}/org.kde.konsole.Window.xml
                    ViewManager.h
                    Konsole::ViewManager)

set(konsoleprivate_SRCS ${sessionadaptors_SRCS}
                        ${windowadaptors_SRCS}
                        BookmarkHandler.cpp
                        BookmarkMenu.cpp
                        ColorScheme.cpp
                        ColorSchemeWallpaper.cpp
                        ColorSchemeManager.cpp
                        ColorSchemeEditor.cpp
                        CheckableSessionModel.cpp
                        CopyInputDialog.cpp
                        EditProfileDialog.cpp
                        ColorSchemeViewDelegate.cpp
                        Emulation.cpp
                        filterHotSpots/FilterChain.cpp
                        filterHotSpots/Filter.cpp
                        filterHotSpots/HotSpot.cpp
                        filterHotSpots/RegExpFilter.cpp
                        filterHotSpots/RegExpFilterHotspot.cpp
                        filterHotSpots/TerminalImageFilterChain.cpp
                        filterHotSpots/UrlFilter.cpp
                        filterHotSpots/UrlFilterHotspot.cpp
                        history/HistoryFile.cpp
                        history/HistoryScroll.cpp
                        history/HistoryScrollFile.cpp
                        history/HistoryScrollNone.cpp
                        CharacterFormat.cpp
                        history/compact/CompactHistoryBlock.cpp
                        history/compact/CompactHistoryBlockList.cpp
                        history/compact/CompactHistoryLine.cpp
                        history/compact/CompactHistoryScroll.cpp
                        history/HistoryType.cpp
                        history/HistoryTypeNone.cpp
                        history/HistoryTypeFile.cpp
                        history/compact/CompactHistoryType.cpp
                        HistorySizeDialog.cpp
                        widgets/HistorySizeWidget.cpp
                        widgets/IncrementalSearchBar.cpp
                        KeyBindingEditor.cpp
                        KeyboardTranslator.cpp
                        FallbackKeyboardTranslator.cpp
                        KeyboardTranslatorReader.cpp
                        KeyboardTranslatorWriter.cpp
                        KeyboardTranslatorManager.cpp
                        ProcessInfo.cpp
                        NullProcessInfo.cpp
                        UnixProcessInfo.cpp
                        SSHProcessInfo.cpp
                        Profile.cpp
                        ProfileList.cpp
                        ProfileReader.cpp
                        ProfileWriter.cpp
                        ProfileManager.cpp
                        Pty.cpp
                        RenameTabDialog.cpp
                        widgets/RenameTabWidget.cpp
                        SaveHistoryTask.cpp
                        SearchHistoryTask.cpp
                        Screen.cpp
                        ScreenWindow.cpp
                        session/Session.cpp
                        session/SessionGroup.cpp
                        session/SessionController.cpp
                        session/SessionManager.cpp
                        session/SessionListModel.cpp
                        session/SessionTask.cpp
                        ShellCommand.cpp
                        widgets/TabTitleFormatButton.cpp
                        TerminalCharacterDecoder.cpp
                        ExtendedCharTable.cpp
                        widgets/TerminalDisplay.cpp
                        widgets/TerminalDisplayAccessible.cpp
                        LineBlockCharacters.cpp
                        widgets/ViewContainer.cpp
                        widgets/ViewContainerTabBar.cpp
                        ViewManager.cpp
                        ViewProperties.cpp
                        widgets/ViewSplitter.cpp
                        Vt102Emulation.cpp
                        ZModemDialog.cpp
                        PrintOptions.cpp
                        WindowSystemInfo.cpp
                        CharacterWidth.cpp
                        AutoScrollHandler.cpp
                        session/SessionDisplayConnection.cpp
                        ${CMAKE_CURRENT_BINARY_DIR}/org.kde.konsole.Window.xml
                        ${CMAKE_CURRENT_BINARY_DIR}/org.kde.konsole.Session.xml)

kde4_add_kcfg_files(konsoleprivate_SRCS settings/KonsoleSettings.kcfgc)

set(konsole_LIBS ${KDE4_KPTY_LIBS}
                 ${KDE4_KIO_LIBS}
                 ${KDE4_KNOTIFYCONFIG_LIBS}
                 Qt4::QtXml
                 ${X11_LIBRARIES})

if(HAVE_LIBKONQ)
  set(konsole_LIBS ${konsole_LIBS} ${LIBKONQ_LIBRARY})
endif()

### Konsole Application

kde4_add_ui_files(konsoleprivate_SRCS ColorSchemeEditor.ui
                                      CopyInputDialog.ui
                                      EditProfileDialog.ui
                                      KeyBindingEditor.ui
                                      RenameTabDialog.ui
                                      widgets/RenameTabWidget.ui
                                      HistorySizeDialog.ui
                                      widgets/HistorySizeWidget.ui
                                      PrintOptions.ui
                                      settings/GeneralSettings.ui
                                      settings/PartInfo.ui
                                      settings/ProfileSettings.ui
                                      settings/TabBarSettings.ui)

kde4_add_library(konsoleprivate SHARED ${konsoleprivate_SRCS})
target_link_libraries(konsoleprivate ${konsole_LIBS})
target_link_libraries(konsoleprivate LINK_INTERFACE_LIBRARIES ${konsole_LIBS})

set(konsole_KDEINIT_SRCS
   Application.cpp
   MainWindow.cpp
   main.cpp
   settings/GeneralSettings.cpp
   settings/ProfileSettings.cpp
   settings/TabBarSettings.cpp)

kde4_add_app_icon(konsole_KDEINIT_SRCS "${KDE4_ICON_INSTALL_DIR}/oxygen/*/apps/utilities-terminal.png")

kde4_add_kdeinit_executable(konsole ${konsole_KDEINIT_SRCS})
target_link_libraries(kdeinit_konsole konsoleprivate)

install(TARGETS kdeinit_konsole konsole konsoleprivate
       ${INSTALL_TARGETS_DEFAULT_ARGS})

### Embedded Konsole KPart

set(konsolepart_PART_SRCS Part.cpp
    settings/PartInfo.cpp
    settings/ProfileSettings.cpp)

kde4_add_plugin(konsolepart WITH_PREFIX ${konsolepart_PART_SRCS})
set_target_properties(konsolepart PROPERTIES DEFINE_SYMBOL KONSOLE_PART)
target_link_libraries(konsolepart ${KDE4_KPARTS_LIBS} konsoleprivate)
install(TARGETS konsolepart  DESTINATION ${PLUGIN_INSTALL_DIR})
