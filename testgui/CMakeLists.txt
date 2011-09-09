######################################################
# CMake Build Script for X-Keys SDK
#
# Alan Ott
# Signal 11 Software
# 2011-08-11
######################################################

project(pietestgui)

cmake_minimum_required(VERSION 2.4.0)
if(COMMAND cmake_policy)
  cmake_policy(SET CMP0003 NEW)
endif(COMMAND cmake_policy)

find_package(Qt4 REQUIRED)

# Source (cpp) files
SET(SRCS
	main.cpp
	MainWindow.cpp
	MainWindow.h
)

SOURCE_GROUP("MOC Files" REGULAR_EXPRESSION moc_.*cxx$)


# Header Files
SET(MOC_HEADERS
	MainWindow.h
)
QT4_WRAP_CPP(MOC_SRCS ${MOC_HEADERS})

# UI Files
SET(UI_FILES
	resources/MainWindow.ui
)
QT4_WRAP_UI(UI_SRCS ${UI_FILES})

# Resource Files
SET(RESOURCE_RC
	resources/MainWindow.qrc
)
QT4_ADD_RESOURCES(RC_SRCS ${RESOURCE_RC})

include(${QT_USE_FILE})

INCLUDE_DIRECTORIES(
	${QT_INCLUDE_DIR}
	${CMAKE_CURRENT_BINARY_DIR}   # This is where the .ui files get generated to.
	../3rdParty/tinyxml
	../piehid
)

LINK_DIRECTORIES(
	${CMAKE_CURRENT_BINARY_DIR}/piehid
)

ADD_DEFINITIONS(-Wall)

ADD_EXECUTABLE(pietestgui ${MOC_SRCS} ${SRCS} ${UI_SRCS} ${UI_SRCS} ${RC_SRCS})


if (WIN32)
   # Make it start in main() on Windows.
   Set_Target_Properties(ios  PROPERTIES
      LINK_FLAGS /ENTRY:"mainCRTStartup")
   
   # Disable the stdlib warnings on Windows.
   ADD_DEFINITIONS(/wd4996)
   
   SET(CMAKE_EXE_LINKER_FLAGS_RELEASE "/subsystem:windows")
	
endif (WIN32)

TARGET_LINK_LIBRARIES(
	pietestgui
	${QT_LIBRARIES}
	piehid
)
