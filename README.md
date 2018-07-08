# Little FTPD (lftpd)

A tiny embeddable ftp server written in C for small embedded targets.
Supports only the minimum neccessary to move files to and from your
device with common FTP clients.


# Features

* IPv4 / IPv6.
* Passive and Extended Passive Modes.
* Works out of the box on POSIX like targets.
* No external dependencies.
* Clear C99 code without anything fancy. Easy to understand and modify.


# Limitations

* One connection at a time.
* No active mode support - PASV and EPSV only.
* Single directory currently - CWD planned.


# Build

Try `make` to build a command line server on any POSIX like OS.


# Embed

A `component.mk` for ESP32 is included. Just put this folder in your
`components` directory.
 