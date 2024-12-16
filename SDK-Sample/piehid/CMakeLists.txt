######################################################
# CMake Build Script for X-Keys SDK
#
# Alan Ott
# Signal 11 Software
# 2011-08-11
######################################################

cmake_minimum_required(VERSION 3.9)

project(piehid VERSION 1.0.0 DESCRIPTION "Xkeys API")

include(GNUInstallDirs)

#if(COMMAND cmake_policy)
#  cmake_policy(SET CMP0003 NEW)
#endif(COMMAND cmake_policy)

if(DEFINED LIBDIRSUFFIX)
  set(LIB_SUFFIX ${LIBDIRSUFFIX})
endif(DEFINED LIBDIRSUFFIX)

# Link against libusb and pthreads.
find_package(PkgConfig)
PKG_CHECK_MODULES(LIBUSB REQUIRED libusb-1.0)
find_package(Threads REQUIRED)

# Source (cpp) files
SET(SRCS
	hid-libusb.c
	PieHid32.c
)

INCLUDE_DIRECTORIES(
	${LIBUSB_INCLUDE_DIRS}
)

LINK_DIRECTORIES(
	${LIBUSB_LIBRARY_DIRS}
)

ADD_DEFINITIONS()

#ADD_EXECUTABLE(test ${SRCS})
ADD_LIBRARY(piehid SHARED ${SRCS})
ADD_LIBRARY(piehid_static STATIC ${SRCS})
SET_TARGET_PROPERTIES(piehid_static PROPERTIES OUTPUT_NAME piehid)

set_target_properties(${CMAKE_PROJECT_NAME}  PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    PUBLIC_HEADER PieHid32.h
)

configure_file(${CMAKE_PROJECT_NAME}.pc.in ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.pc @ONLY)

TARGET_LINK_LIBRARIES(
	piehid
	${LIBUSB_LIBRARIES}
	${CMAKE_THREAD_LIBS_INIT}
)

INSTALL(FILES ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.pc DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/pkgconfig)

INSTALL(TARGETS piehid piehid_static
	RUNTIME DESTINATION bin
	LIBRARY DESTINATION lib${LIB_SUFFIX}
	ARCHIVE DESTINATION lib${LIB_SUFFIX}
	PUBLIC_HEADER DESTINATION include )



