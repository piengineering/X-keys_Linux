X-Keys Linux v1.0.0
===================

This repository contains a sample and tools for using X-keys products on Linux operating systems.

## Building & linking the library

just invoke cmake from the SDK-Sample folder, this will build a sample app, and the HID library.
The HID library can be linked using pkgConfig, using the piedhid.pc package.

## Udev integration

File [udev-61-xkeys.rules.txt](udev-Rules/61-xkeys.rules.txt) maybe bo copied to the __/usr/lib/udev/__ directory on the target device, to allow software access to the raw HID messages and to address descriptor conflicts for the XK-24, XK-80, and XK-128.

## API Specification

Device information and HID Input and Output Data Reports for P.I. Engineering products are located in the HID-Repots folder.
