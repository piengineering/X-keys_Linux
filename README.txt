X-Keys SDK for Linux
PI Engineering
Signal 11 Software

Build instructions:
====================

Prerequisites
--------------
The SDK requires libusb-1.0 in order to build. The GUI test application
requires Qt 4.  The build system is CMake.  To install all of these on an
Ubuntu system, run the following from the command line:

    sudo apt-get install build-essential libqt4-dev libusb-1.0-0-dev cmake

Based on your version of Ubuntu, these packages may have slightly different
names. Use Bash's tab-completion to find the exact name for these packages
on your system. On different distribution of Linux, install a compiler, Qt,
libusb-1.0, and cmake using your distribution's package system or install
them manually from source.

Build
------
run ./configure from this directory. This will configure the CMake build
system and create a build/ directory which will contain all the binaries. 
Run ./configure --help to see a list of common options which can be passed
to the configure script.  All options passed to ./configure are passed
directly to the CMake.

Running "make" in this directory after ./configure will build the software

Running
--------
The binaries are located in the build/ directory and can be run directly
from this directory using the following:
	build/testgui/pietestgui  (the GUI test application)
	build/test/piehidtest     (the console test application).
	build/pedalgui/pedalgui   (the GUI application for the VEC Pedal)

Installation
-------------
Running "make install" from this directory after the software has been built
will install the library.  By default it installs the PieHid32.h header and
the shared and static library files into /usr/local/.  The install directory
can be set from the configure script.  Running
	./configure -D CMAKE_INSTALL_PREFIX=/usr
will cause the installed programs to be installed into /usr instead of
/usr/local/.


Usage
======
In order for /dev entries for X-Keys products to be readable by non-root
users, a udev rule will need to be placed in /etc/udev/rules.d . A sample
udev rule file is located in the udev folder. Simply copy this file to
/etc/udev/rules.d using
	sudo cp udev/90-xkeys.rules /etc/udev/rules.d/
from this folder.
