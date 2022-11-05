# kuemmel
kuemmel is a spice server implementation for non-virtual environments

# [spice](https://www.spice-space.org/)
SPICE (the Simple Protocol for Independent Computing Environments) is a remote desktop protocol(like VNC and RDP).
The main server implementation is QEMU, so it is mainly used for controlling virtual machines. But there is also an X11 spice server and another implementation for sharing an active X11 session.

# Implementation
kuemmel is based on [libspice-server](https://gitlab.freedesktop.org/spice/spice).
For acquiring screen data it uses [Windows Desktop Duplication API](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api).

# Building
The reference build environment is msys2/mingw64.
Glib and libspice-server are available as packages.
CMake and ninja are used for building.

# State
This project is still on proof of concept state.
There is a lot of hacks in the code, e.g. the screen resolution is hard coded.
