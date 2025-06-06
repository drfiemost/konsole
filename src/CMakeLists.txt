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

### Font Embedder and LineFont.h
option(KONSOLE_BUILD_FONTEMBEDDER "Konsole: build fontembedder executable" OFF)
option(KONSOLE_GENERATE_LINEFONT "Konsole: regenerate LineFont file" OFF)

### Use cmake -DKONSOLE_BUILD_FONTEMBEDDER or/and -DKONSOLE_GENERATE_LINEFONT=1
if(KONSOLE_BUILD_FONTEMBEDDER OR KONSOLE_GENERATE_LINEFONT)
  ### Font Embedder
  set(fontembedder_SRCS fontembedder.cpp)
  kde4_add_executable(fontembedder ${fontembedder_SRCS})
  target_link_libraries(fontembedder  ${KDE4_KIO_LIBS})

  ### Line graphics font
  ### The below cmake doesn't work; it might be possible to automate it
  ###   if LineFont.src has changed.  If automated, different OSes will
  ###   need to be handled.
  ###   If LineFont.h is needed to be recreated use:
  ###     fontembedder LineFont.src > LineFont.h
  #add_custom_command(OUTPUT ${CMAKE_SOURCE_DIR}/LineFont.h
  #                   COMMAND ${CMAKE_CURRENT_BINARY_DIR}/fontembedder
  #                   ARGS ${CMAKE_SOURCE_DIR}/LineFont.src
  #                   DEPENDS ${CMAKE_SOURCE_DIR}/LineFont.src )
endif()

### Konsole source files shared between embedded terminal and main application
# qdbuscpp2xml -m  Session.h -o org.kde.konsole.Session.xml
# qdbuscpp2xml -M -s ViewManager.h -o org.kde.konsole.Konsole.xml

# Generate dbus .xml files; do not store .xml in source folder
qt4_generate_dbus_interface(Session.h org.kde.konsole.Session.xml OPTIONS -m)
qt4_generate_dbus_interface(ViewManager.h org.kde.konsole.Window.xml OPTIONS -m)

qt4_add_dbus_adaptor(sessionadaptors_SRCS
                    ${CMAKE_CURRENT_BINARY_DIR}/org.kde.konsole.Session.xml
                    Session.h
                    Konsole::Session)
qt4_add_dbus_adaptor(windowadaptors_SRCS
                    ${CMAKE_CURRENT_BINARY_DIR}/org.kde.konsole.Window.xml
                    ViewManager.h
                    Konsole::ViewManager)

set(konsoleprivate_SRCS ${sessionadaptors_SRCS}
                        ${windowadaptors_SRCS}
                        BookmarkHandler.cpp
                        ColorScheme.cpp
                        ColorSchemeManager.cpp
                        ColorSchemeEditor.cpp
                        CopyInputDialog.cpp
                        EditProfileDialog.cpp
                        Emulation.cpp
                        Filter.cpp
                        History.cpp
                        HistorySizeDialog.cpp
                        HistorySizeWidget.cpp
                        IncrementalSearchBar.cpp
                        KeyBindingEditor.cpp
                        KeyboardTranslator.cpp
                        KeyboardTranslatorManager.cpp
                        ManageProfilesDialog.cpp
                        ProcessInfo.cpp
                        Profile.cpp
                        ProfileList.cpp
                        ProfileReader.cpp
                        ProfileWriter.cpp
                        ProfileManager.cpp
                        Pty.cpp
                        RenameTabDialog.cpp
                        RenameTabWidget.cpp
                        Screen.cpp
                        ScreenWindow.cpp
                        Session.cpp
                        SessionController.cpp
                        SessionManager.cpp
                        SessionListModel.cpp
                        ShellCommand.cpp
                        TabTitleFormatButton.cpp
                        TerminalCharacterDecoder.cpp
                        ExtendedCharTable.cpp
                        TerminalDisplay.cpp
                        TerminalDisplayAccessible.cpp
                        ViewContainer.cpp
                        ViewContainerTabBar.cpp
                        ViewManager.cpp
                        ViewProperties.cpp
                        ViewSplitter.cpp
                        Vt102Emulation.cpp
                        ZModemDialog.cpp
                        PrintOptions.cpp
                        konsole_wcwidth.cpp
                        WindowSystemInfo.cpp
                        ${CMAKE_CURRENT_BINARY_DIR}/org.kde.konsole.Window.xml
                        ${CMAKE_CURRENT_BINARY_DIR}/org.kde.konsole.Session.xml)
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
                                      ManageProfilesDialog.ui
                                      RenameTabDialog.ui
                                      RenameTabWidget.ui
                                      HistorySizeDialog.ui
                                      HistorySizeWidget.ui
                                      PrintOptions.ui
                                      settings/GeneralSettings.ui
                                      settings/TabBarSettings.ui)

kde4_add_library(konsoleprivate SHARED ${konsoleprivate_SRCS})
target_link_libraries(konsoleprivate ${konsole_LIBS})
target_link_libraries(konsoleprivate LINK_INTERFACE_LIBRARIES ${konsole_LIBS})

set(konsole_KDEINIT_SRCS
   Application.cpp
   MainWindow.cpp
   main.cpp
   settings/GeneralSettings.cpp
   settings/TabBarSettings.cpp)

kde4_add_kcfg_files(konsole_KDEINIT_SRCS settings/KonsoleSettings.kcfgc)

kde4_add_app_icon(konsole_KDEINIT_SRCS "${KDE4_ICON_INSTALL_DIR}/oxygen/*/apps/utilities-terminal.png")

kde4_add_kdeinit_executable(konsole ${konsole_KDEINIT_SRCS})
target_link_libraries(kdeinit_konsole konsoleprivate)

install(TARGETS kdeinit_konsole konsole konsoleprivate
       ${INSTALL_TARGETS_DEFAULT_ARGS})

### Embedded Konsole KPart

set(konsolepart_PART_SRCS Part.cpp)

kde4_add_plugin(konsolepart WITH_PREFIX ${konsolepart_PART_SRCS})
set_target_properties(konsolepart PROPERTIES DEFINE_SYMBOL KONSOLE_PART)
target_link_libraries(konsolepart ${KDE4_KPARTS_LIBS} konsoleprivate)
install(TARGETS konsolepart  DESTINATION ${PLUGIN_INSTALL_DIR})

### konsoleprofile command-line tool
install(PROGRAMS konsoleprofile DESTINATION ${BIN_INSTALL_DIR})

