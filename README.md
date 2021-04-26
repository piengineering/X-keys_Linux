## X-Keys SDK for Linux

PI Engineering, Signal 11 Software

### Build instructions:

#### Prerequisites

The SDK requires libusb-1.0 in order to build. The GUI test application
requires Qt 5.  The build system is CMake.  To install all of these dependencies
run the appropriate command for your operating system/distro:
```
    # Debian 8 / 9 / 10
    sudo apt-get install -y build-essential qtbase5-dev libusb-1.0-0-dev cmake
    
    # Ubuntu 16.04+ (tested through version 20.10)
    sudo apt-get install -y build-essential qtbase5-dev libusb-1.0-0-dev cmake pkg-config 

    # RHEL 7 / CentOS 7 / Oracle 7
    sudo yum group install -y "Development Tools" && sudo yum install -y qt5-qtbase-devel cmake git libusbx-devel xorg-x11-xauth dbus-x11
    
    # CentOS 8 / CentOS 8 / Oracle 8
    sudo dnf group install -y "Development Tools" && sudo dnf install -y qt5-qtbase-devel cmake git libusbx-devel xorg-x11-xauth dbus-x11
```
These packages may have slightly different names on other operating systems
and/or distro versions.

#### Clone

To acquire the souce code cloen the Xkeys repo and change into the cloned
directory;

```
    git clone https://github.com/piengineering/X-keys_Linux && cd X-keys_Linux 
 ```

#### Build

Run `./configure` from cloned directory. This will configure the CMake build
system and create a build/ directory which will contain all the binaries. 
Run `./configure --help` to see a list of common options which can be passed
to the configure script.  All options passed to `./configure` are passed
directly to the CMake. If .`/configure` succeeds, run `make` in the same
directory to build the software:

```
    # On RHEL 7 / CentOS 7 / Oracle 7
    ./configure -DCMAKE_CXX_FLAGS=--std=c++11 && make
    
    # On Debian / Ubuntu or RHEL 8 / CentOS 8 / Oracle 8
    ./configure && make
```

#### Running

The binaries are located in the build/ directory and can be run directly
from this directory using the following:
```
    build/testgui/pietestgui  (the GUI test application)
    build/test/piehidtest     (the console test application).
```

## Installation:

Running `make install` from this directory after the software has been built
will install the library.  By default it installs the PieHid32.h header and
the shared and static library files into /usr/local/.  The set a different 
directory prefix, you can can run:
```
    # To install in /usr/local
    ./configure && sudo make install
    
    # To install in /usr
    ./configure -D CMAKE_INSTALL_PREFIX=/usr && sudo make install
```

## Usage:

In order for /dev entries for X-Keys products to be readable by non-root
users, a udev rule will need to be placed in /etc/udev/rules.d . A sample
udev rule file is located in the udev folder. Simply copy this file to
/etc/udev/rules.d using:
```
    sudo cp udev/90-xkeys.rules /etc/udev/rules.d/
```
from this folder.
