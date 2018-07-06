# Little FTPD (lftpd)

A tiny embeddable ftp server written in C for small embedded targets.
Supports only the minimum neccessary to move files to and from your
device with common FTP clients.

# Features

* IPv4 / IPv6.
* Passive and Extended Passive Modes.
* Works out of the box on POSIX like targets.
* All platform specific functions can be overridden to easily port to
	any target.
* No external dependencies.
* Clear C99 code without anything fancy. Easy to understand and modify.

# Build

Try `make` to build a command line server on any POSIX like OS.

# Embed

A `component.mk` for ESP32 is included. Just put this folder in your
`components` directory.
 