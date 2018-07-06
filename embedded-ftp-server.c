#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#include "private/efs_log.h"
#include "private/efs_inet.h"
#include "private/efs_string.h"

// https://tools.ietf.org/html/rfc959
// https://tools.ietf.org/html/rfc2389#section-2.2
// https://tools.ietf.org/html/rfc3659
// https://tools.ietf.org/html/rfc5797
// https://tools.ietf.org/html/rfc2428#section-3 EPSV
// https://en.wikipedia.org/wiki/List_of_FTP_commands
// https://github.com/toelke/lwip-ftpd/blob/master/ftpd.c

// TODO Get rid of stack buffers, pass them in
// TODO Make sure all commands in the enum are implemented
// TODO Print out information about where we are listening
// TODO Break IO, utils, etc. out

typedef enum {
	ASCII,
	BINARY,
} transfer_type_t;

typedef struct {
	const char* directory;
	int socket;
	int data_socket;
	char buffer[256];
} client_t;

typedef struct {
	char *command;
	int (*handler) (client_t* client, const char* arg);
} command_t;

//      In order to make FTP workable without needless error messages, the
//      following minimum implementation is required for all servers:
//
//         TYPE - ASCII Non-print
//         MODE - Stream
//         STRUCTURE - File, Record
//         COMMANDS - USER, QUIT, PORT,
//                    TYPE, MODE, STRU,
//                      for the default values
//                    RETR, STOR,
//                    NOOP.
//
//      The default values for transfer parameters are:
//
//         TYPE - ASCII Non-print
//         MODE - Stream
//         STRU - File
//
//      All hosts must accept the above as the standard defaults.

static int cmd_cwd();
static int cmd_dele();
static int cmd_epsv();
static int cmd_feat();
static int cmd_list();
static int cmd_noop();
static int cmd_pass();
static int cmd_pasv();
static int cmd_pwd();
static int cmd_quit();
static int cmd_retr();
static int cmd_stat();
static int cmd_stor();
static int cmd_syst();
static int cmd_type();
static int cmd_user();

static command_t commands[] = {
	{ "CWD", cmd_cwd },
	{ "DELE", cmd_dele },
	{ "EPSV", cmd_epsv },
	{ "FEAT", cmd_feat },
	{ "LIST", cmd_list },
	{ "NOOP", cmd_noop },
	{ "PASS", cmd_pass },
	{ "PASV", cmd_pasv },
	{ "PWD", cmd_pwd },
	{ "QUIT", cmd_quit },
	{ "RETR", cmd_retr },
//	{ "STAT", cmd_stat },
	{ "STOR", cmd_stor },
	{ "SYST", cmd_syst },
	{ "TYPE", cmd_type },
	{ "USER", cmd_user },
	{ NULL, NULL },
};

/**
 * @brief Write a simple response of the form NNN ssss...\r\n to the client.
 * For example, write_simple_response(client, 220, "Welcome") will write
 * "220 Welcome\r\n" to the client.
 */
// TODO refactor to take format - it just makes everything easier
// and then see if we can get rid of the rest of the stuff below
static int write_simple_response(client_t* client, int code, const char* message) {
	return efs_inet_writef(
			client->socket,
			client->buffer,
			sizeof(client->buffer),
			"%d %s%s", code, message, CRLF);
}

// See 4.2.  FTP REPLIES
// It's really a multi-line response, so think about how that works
// Maybe start/write/end
static int write_continue_response(client_t* client, int code, const char* message) {
	return efs_inet_writef(
			client->socket,
			client->buffer,
			sizeof(client->buffer),
			"%d-%s%s", code, message, CRLF);
}

static int start_multiline_response(client_t* client, int code, const char* message) {
	return efs_inet_writef(
			client->socket,
			client->buffer,
			sizeof(client->buffer),
			"%d-%s%s", code, message, CRLF);
}

static int continue_multiline_response(client_t* client, const char* message) {
	return efs_inet_writef(
			client->socket,
			client->buffer,
			sizeof(client->buffer),
			"%s%s", message, CRLF);
}

static int end_multiline_response(client_t* client, int code, const char* message) {
	return write_simple_response(client, code, message);
}

static int write_directory_listing(int socket, const char* directory) {
	DIR* dp = opendir(directory);
	if (dp == NULL) {
		return -1;
	}

	// TODO error checking
	struct dirent *entry;
	while ((entry = readdir(dp))) {
		// TODO fixed buffer, string horribleness
		char path[256];
		strcpy(path, directory);
		strcat(path, "/");
		strcat(path, entry->d_name);
		struct stat st;
		off_t size = 0;
		if (stat(path, &st) == 0) {
			size = st.st_size;
		}
		// TODO fixed buffer
		char buffer[256];
		// https://files.stairways.com/other/ftp-list-specs-info.txt
		// http://cr.yp.to/ftp/list/binls.html
		efs_inet_writef(socket,
				buffer,
				sizeof(buffer),
				"-rw-rw-rw- 1 owner group %13llu Jan 01  1970 %s%s", size, entry->d_name, CRLF);
	}

	closedir(dp);

	return 0;
}

static int cmd_cwd(client_t* client, const char* arg) {
	write_simple_response(client, 250, "OK.");
	return 0;
}

static int cmd_dele(client_t* client, const char* arg) {
	return 0;
}

static int cmd_epsv(client_t* client, const char* arg) {
	// open a data port
	int listener_socket = efs_inet_listen(0);
	if (listener_socket < 0) {
		// TODO find proper result code
		write_simple_response(client, 500, "Error: Unable to open data port.");
		return -1;
	}

	// get the port from the new socket, which is random
	int port = efs_inet_get_socket_port(listener_socket);

	// format the response
	efs_inet_writef(client->socket, client->buffer, sizeof(client->buffer),
			"229 Entering Extended Passive Mode (|||%d|).%s", port, CRLF);

	// wait for the connection to the data port
	efs_log_info("waiting for data port connection on port %d...", port);
	int client_socket = accept(listener_socket, NULL, NULL);
	if (client_socket < 0) {
		efs_log_error("error accepting client socket");
		return -1;
	}
	efs_log_info("data port connection received...");

	// close the listener
	close(listener_socket);

	client->data_socket = client_socket;

	return 0;
}

static int cmd_feat(client_t* client, const char* arg) {
	start_multiline_response(client, 211, "Features:");
	continue_multiline_response(client, "EPSV");
	continue_multiline_response(client, "PASV");
	continue_multiline_response(client, "SIZE");
	end_multiline_response(client, 211, "End");
	return 0;
}

static int cmd_list(client_t* client, const char* arg) {
	write_simple_response(client, 150, "Here comes the directory listing.");
	write_directory_listing(client->data_socket, client->directory);
	close(client->data_socket);
	client->data_socket = -1;
	write_simple_response(client, 226, "Directory send OK.");
	return 0;
}

static int cmd_noop(client_t* client, const char* arg) {
	write_simple_response(client, 200, "OK.");
	return 0;
}

static int cmd_pass(client_t* client, const char* arg) {
	write_simple_response(client, 230, "Login successful.");
	return 0;
}

static int cmd_pasv(client_t* client, const char* arg) {
	// open a data port
	int listener_socket = efs_inet_listen(0);
	if (listener_socket < 0) {
		// TODO find proper result code
		write_simple_response(client, 500, "Error: Unable to open data port.");
		return -1;
	}

	// get the port from the new socket, which is random
	int port = efs_inet_get_socket_port(listener_socket);

	// format the response
	// TODO figure out what to return for the IP
	efs_inet_writef(client->socket, client->buffer, sizeof(client->buffer),
			"227 Entering Passive Mode (%d,%d,%d,%d,%d,%d).%s",
			0, 0, 0, 0,
			(port >> 8) & 0xff, (port >> 0) & 0xff,
			CRLF);

	// wait for the connection to the data port
	efs_log_info("waiting for data port connection on port %d...", port);
	int client_socket = accept(listener_socket, NULL, NULL);
	if (client_socket < 0) {
		efs_log_error("error accepting client socket");
		return -1;
	}
	efs_log_info("data port connection received...");

	// close the listener
	close(listener_socket);

	client->data_socket = client_socket;

	return 0;
	return 0;
}

static int cmd_pwd(client_t* client, const char* arg) {
	efs_inet_writef(client->socket,
			client->buffer,
			sizeof(client->buffer),
			"%d \"%s\"%s", 257, client->directory, CRLF);
	return 0;
}

static int cmd_quit(client_t* client, const char* arg) {
	write_simple_response(client, 221, "Bye.");
	return -1;
}

static int cmd_retr(client_t* client, const char* arg) {
	return 0;
}


static int cmd_stat(client_t* client, const char* arg) {
	if (arg) {
		// status of directory or file
		// STAT /
		// 213-Status follows:
		// drwxr-xr-x    3 0        108          4096 Mar 28  2015 .
		// drwxr-xr-x    3 0        108          4096 Mar 28  2015 ..
		// -rw-r--r--    1 0        0        1073741824000 Feb 19  2016 1000GB.zip
		// -rw-r--r--    1 0        0        107374182400 Feb 19  2016 100GB.zip
		// -rw-r--r--    1 0        0         5242880 Feb 19  2016 5MB.zip
		// drwxr-xr-x    2 105      108         12288 Jul 06 02:28 upload
		// 213 End of status
	}
	else {
		// status of server
		//STAT
		//211-FTP server status:
		//     Connected to ::ffff:136.61.85.70
		//     Logged in as ftp
		//     TYPE: ASCII
		//     No session bandwidth limit
		//     Session timeout in seconds is 300
		//     Control connection is plain text
		//     Data connections will be plain text
		//     At session startup, client count was 30
		//     vsFTPd 2.3.5 - secure, fast, stable
		//211 End of status
		start_multiline_response(client, 211, "FTP server status:");
		end_multiline_response(client, 211, "End of status");
	}
	return 0;
}

static int cmd_stor(client_t* client, const char* arg) {
	return 0;
}

static int cmd_syst(client_t* client, const char* arg) {
	write_simple_response(client, 215, "UNIX Type: L8");
	return 0;
}

static int cmd_type(client_t* client, const char* arg) {
	write_simple_response(client, 200, "OK.");
	return 0;
}

static int cmd_user(client_t* client, const char* arg) {
	write_simple_response(client, 230, "Login successful.");
	return 0;
}

static int handle_control_channel(client_t* client) {
	int err = write_simple_response(client, 220, "Welcome to embedded-ftp-server (v1.0).");
	if (err != 0) {
		efs_log_error("error sending welcome message");
		goto cleanup;
	}

	while (err == 0) {
		int line_len = efs_inet_read_line(client->socket, client->buffer, sizeof(client->buffer));
		if (line_len != 0) {
			efs_log_error("error reading next command");
			goto cleanup;
		}

		// find the index of the first space
		int index;
		char* p = strchr(client->buffer, ' ');
		if (p != NULL) {
			index = p - client->buffer;
		}
		// if no space, use the whole string
		else {
			index = strlen(client->buffer);
		}

		// if the index is 5 or greater the command is too long
		if (index >= 5) {
			efs_log_error("invalid command, too long (%d)", index);
			return -1;
		}

		// copy the command into a temporary buffer
		char command_tmp[4 + 1];
		memset(command_tmp, 0, sizeof(command_tmp));
		memcpy(command_tmp, client->buffer, index);

		// upper case the command
		for (int i = 0; command_tmp[i]; i++) {
			command_tmp[i] = (char) toupper((int) command_tmp[i]);
		}

		// see if we have a matching function for the command, and if
		// so, dispatch it
		bool matched = false;
		for (int i = 0; commands[i].command; i++) {
			if (strcmp(commands[i].command, command_tmp) == 0) {
				// TODO arg
				// TODO return value
				commands[i].handler(client, NULL);
				matched = true;
				break;
			}
		}
		if (!matched) {
			write_simple_response(client, 502, "Unsupported.");
		}
	}

	cleanup:
	close(client->socket);

	return 0;
}

int efs_start(const char* directory, int port) {
	int server_socket = efs_inet_listen(port);
	if (server_socket < 0) {
		efs_log_error("error creating listener");
		return -1;
	}

	while (true) {
		efs_log_info("waiting for connection...");

		int client_socket = accept(server_socket, NULL, NULL);
		if (client_socket < 0) {
			efs_log_error("error accepting client socket");
			continue;
		}

		struct sockaddr_in6 client_addr;
		socklen_t client_addr_len = sizeof(struct sockaddr_in6);
		int err = getsockname(client_socket, (struct sockaddr*) &client_addr, &client_addr_len);
		if (err != 0) {
			efs_log_error("error getting client IP info");
			efs_log_info("connection received...");
		}
		else {
			char buffer[INET6_ADDRSTRLEN];
			inet_ntop(AF_INET6, &client_addr.sin6_addr, buffer, INET6_ADDRSTRLEN);
			efs_log_info("connection received from %s...", buffer);
		}

		client_t client = {
				.directory = directory,
				.socket = client_socket,
				.data_socket = -1,
		};
		handle_control_channel(&client);
	}

	return 0;
}

int main( int argc, char *argv[] ) {
	char* cwd = getcwd(NULL, 0);
	efs_start(cwd, 2121);
	free(cwd);
}

