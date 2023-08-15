######################################################
# CMake Build Script for X-Keys SDK
#
# Alan Ott
# Signal 11 Software
# 2011-08-11
######################################################

project(piehid)

cmake_minimum_required(VERSION 2.8.0)
if(COMMAND cmake_policy)
  cmake_policy(SET CMP0003 NEW)
endif(COMMAND cmake_policy)

if(DEFINED LIBDIRSUFFIX)
  set(LIB_SUFFIX ${LIBDIRSUFFIX})
endif(DEFINED LIBDIRSUFFIX)

OPTION(BUILD_STATIC_LIBS "Build static library" ON)

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

ADD_LIBRARY(piehid SHARED ${SRCS})
IF(BUILD_STATIC_LIBS)
    ADD_LIBRARY(piehid_static STATIC ${SRCS})
    SET_TARGET_PROPERTIES(piehid_static PROPERTIES OUTPUT_NAME piehid)
ENDIF (BUILD_STATIC_LIBS)
SET_TARGET_PROPERTIES(piehid PROPERTIES PUBLIC_HEADER PieHid32.h)
SET_TARGET_PROPERTIES(piehid PROPERTIES SOVERSION 1)

TARGET_LINK_LIBRARIES(
	piehid
	${LIBUSB_LIBRARIES}
	${CMAKE_THREAD_LIBS_INIT}
)


IF(BUILD_STATIC_LIBS)
    INSTALL(TARGETS piehid piehid_static
    	RUNTIME DESTINATION bin
    	LIBRARY DESTINATION lib${LIB_SUFFIX}
    	ARCHIVE DESTINATION lib${LIB_SUFFIX}
    	PUBLIC_HEADER DESTINATION include )
ELSE()
    INSTALL(TARGETS piehid
    	RUNTIME DESTINATION bin
    	LIBRARY DESTINATION lib${LIB_SUFFIX}
    	PUBLIC_HEADER DESTINATION include )
ENDIF (BUILD_STATIC_LIBS)

