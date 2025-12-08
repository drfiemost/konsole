
### konsoleprofile command-line tool
install(PROGRAMS konsoleprofile DESTINATION ${BIN_INSTALL_DIR})

### Line graphics font
###  Attempting to auto-create LineFont.h for multiple systems is a headache.
###  If LineFont.h is needed to be recreated use:
###     fontembedder LineFont.src > LineFont.h
###  Then commit the new LineFont.h
if(KONSOLE_BUILD_FONTEMBEDDER OR KONSOLE_GENERATE_LINEFONT)
    find_package(Qt4 REQUIRED)

    ### Font Embedder
    set(fontembedder_SRCS fontembedder.cpp)
    add_executable(fontembedder ${fontembedder_SRCS})
    target_link_libraries(fontembedder Qt4::QtCore)
endif()
