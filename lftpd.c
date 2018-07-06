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

#include "private/lftpd_inet.h"
#include "private/lftpd_log.h"
#include "private/lftpd_string.h"

// https://tools.ietf.org/html/rfc959
// https://tools.ietf.org/html/rfc2389#section-2.2
// https://tools.ietf.org/html/rfc3659
// https://tools.ietf.org/html/rfc5797
// https://tools.ietf.org/html/rfc2428#section-3 EPSV
// https://en.wikipedia.org/wiki/List_of_FTP_commands
// https://github.com/toelke/lwip-ftpd/blob/master/ftpd.c

// TODO Get rid of stack buffers, pass them in
// TODO Print out information about where we are listening
// TODO Change response text to the same as in the spec

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
static int send_simple_response(client_t* client, int code, const char* message) {
	return lftpd_inet_writef(
			client->socket,
			client->buffer,
			sizeof(client->buffer),
			"%d %s%s", code, message, CRLF);
}

static int send_multiline_response_begin(client_t* client, int code, const char* message) {
	return lftpd_inet_writef(
			client->socket,
			client->buffer,
			sizeof(client->buffer),
			"%d-%s%s", code, message, CRLF);
}

static int send_multiline_response_line(client_t* client, const char* message) {
	return lftpd_inet_writef(
			client->socket,
			client->buffer,
			sizeof(client->buffer),
			"%s%s", message, CRLF);
}

static int send_multiline_response_end(client_t* client, int code, const char* message) {
	return send_simple_response(client, code, message);
}

static int send_directory_listing(int socket, const char* directory) {
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
		lftpd_inet_writef(socket,
				buffer,
				sizeof(buffer),
				"-rw-rw-rw- 1 owner group %13llu Jan 01  1970 %s%s", size, entry->d_name, CRLF);
	}

	closedir(dp);

	return 0;
}

static int cmd_cwd(client_t* client, const char* arg) {
	if (arg && strcmp("/", arg) == 0) {
		send_simple_response(client, 250, "OK.");
	}
	else {
		send_simple_response(client, 500, "Invalid directory.");
	}
	return 0;
}

static int cmd_dele(client_t* client, const char* arg) {
	return 0;
}

static int cmd_epsv(client_t* client, const char* arg) {
	// open a data port
	int listener_socket = lftpd_inet_listen(0);
	if (listener_socket < 0) {
		// TODO find proper result code
		send_simple_response(client, 500, "Error: Unable to open data port.");
		return -1;
	}

	// get the port from the new socket, which is random
	int port = lftpd_inet_get_socket_port(listener_socket);

	// format the response
	lftpd_inet_writef(client->socket, client->buffer, sizeof(client->buffer),
			"229 Entering Extended Passive Mode (|||%d|).%s", port, CRLF);

	// wait for the connection to the data port
	lftpd_log_info("waiting for data port connection on port %d...", port);
	int client_socket = accept(listener_socket, NULL, NULL);
	if (client_socket < 0) {
		lftpd_log_error("error accepting client socket");
		return -1;
	}
	lftpd_log_info("data port connection received...");

	// close the listener
	close(listener_socket);

	client->data_socket = client_socket;

	return 0;
}

static int cmd_feat(client_t* client, const char* arg) {
	send_multiline_response_begin(client, 211, "Features:");
	send_multiline_response_line(client, "EPSV");
	send_multiline_response_line(client, "PASV");
	send_multiline_response_line(client, "SIZE");
	send_multiline_response_end(client, 211, "End");
	return 0;
}

static int cmd_list(client_t* client, const char* arg) {
	if (client->data_socket == -1) {
		send_simple_response(client, 425, "Use PASV or EPSV first.");
		return -1;
	}
	send_simple_response(client, 150, "Here comes the directory listing.");
	send_directory_listing(client->data_socket, client->directory);
	close(client->data_socket);
	client->data_socket = -1;
	send_simple_response(client, 226, "Directory send OK.");
	return 0;
}

static int cmd_noop(client_t* client, const char* arg) {
	send_simple_response(client, 200, "OK.");
	return 0;
}

static int cmd_pass(client_t* client, const char* arg) {
	send_simple_response(client, 230, "Login successful.");
	return 0;
}

static int cmd_pasv(client_t* client, const char* arg) {
	// open a data port
	int listener_socket = lftpd_inet_listen(0);
	if (listener_socket < 0) {
		// TODO find proper result code
		send_simple_response(client, 500, "Error: Unable to open data port.");
		return -1;
	}

	// get the port from the new socket, which is random
	int port = lftpd_inet_get_socket_port(listener_socket);

	// format the response
	// TODO figure out what to return for the IP
	lftpd_inet_writef(client->socket, client->buffer, sizeof(client->buffer),
			"227 Entering Passive Mode (%d,%d,%d,%d,%d,%d).%s",
			0, 0, 0, 0,
			(port >> 8) & 0xff, (port >> 0) & 0xff,
			CRLF);

	// wait for the connection to the data port
	lftpd_log_info("waiting for data port connection on port %d...", port);
	int client_socket = accept(listener_socket, NULL, NULL);
	if (client_socket < 0) {
		lftpd_log_error("error accepting client socket");
		return -1;
	}
	lftpd_log_info("data port connection received...");

	// close the listener
	close(listener_socket);

	client->data_socket = client_socket;

	return 0;
	return 0;
}

static int cmd_pwd(client_t* client, const char* arg) {
	send_simple_response(client, 257, "/");
	return 0;
}

static int cmd_quit(client_t* client, const char* arg) {
	send_simple_response(client, 221, "Bye.");
	return -1;
}

static int cmd_retr(client_t* client, const char* arg) {
	if (client->data_socket == -1) {
		send_simple_response(client, 425, "Use PASV or EPSV first.");
		return -1;
	}
	send_simple_response(client, 150, "Sending file.");
	// send file to data socket
	// TODO hax
	// TODO strip path
	char* filename = (char*) arg;
	char path[256];
	sprintf(path, "%s/%s", client->directory, filename);
	lftpd_log_info("send '%s'", path);
	FILE* file = fopen(path, "rb");
	if (file == NULL) {
		lftpd_log_error("failed to open file for read");
	}
	unsigned char buffer[1024];
	int d;
	while ((d = fread(buffer, 1024, 1, file)) > 0) {
		write(client->data_socket, buffer, d);
	}
	fclose(file);
	close(client->data_socket);
	client->data_socket = -1;
	send_simple_response(client, 226, "OK.");
	return 0;
}

static int cmd_stor(client_t* client, const char* arg) {
	return 0;
}

static int cmd_syst(client_t* client, const char* arg) {
	send_simple_response(client, 215, "UNIX Type: L8");
	return 0;
}

static int cmd_type(client_t* client, const char* arg) {
	send_simple_response(client, 200, "OK.");
	return 0;
}

static int cmd_user(client_t* client, const char* arg) {
	send_simple_response(client, 230, "Login successful.");
	return 0;
}

static int handle_control_channel(client_t* client) {
	int err = send_simple_response(client, 220, "lftpd.");
	if (err != 0) {
		lftpd_log_error("error sending welcome message");
		goto cleanup;
	}

	while (err == 0) {
		int line_len = lftpd_inet_read_line(client->socket, client->buffer, sizeof(client->buffer));
		if (line_len != 0) {
			lftpd_log_error("error reading next command");
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
			lftpd_log_error("invalid command, too long (%d)", index);
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
				char* arg = NULL;
				if (index < strlen(client->buffer)) {
					// TODO buffer
					char buffer[256];
					strcpy(buffer, client->buffer + index + 1);
					arg = lftpd_string_trim(buffer);
				}
				err = commands[i].handler(client, arg);
				matched = true;
				break;
			}
		}
		if (!matched) {
			send_simple_response(client, 502, "Unsupported.");
		}
	}

	cleanup:
	close(client->socket);

	return 0;
}

int lftpd_start(const char* directory, int port) {
	int server_socket = lftpd_inet_listen(port);
	if (server_socket < 0) {
		lftpd_log_error("error creating listener");
		return -1;
	}

	while (true) {
		lftpd_log_info("waiting for connection...");

		int client_socket = accept(server_socket, NULL, NULL);
		if (client_socket < 0) {
			lftpd_log_error("error accepting client socket");
			continue;
		}

		struct sockaddr_in6 client_addr;
		socklen_t client_addr_len = sizeof(struct sockaddr_in6);
		int err = getsockname(client_socket, (struct sockaddr*) &client_addr, &client_addr_len);
		if (err != 0) {
			lftpd_log_error("error getting client IP info");
			lftpd_log_info("connection received...");
		}
		else {
			char buffer[INET6_ADDRSTRLEN];
			inet_ntop(AF_INET6, &client_addr.sin6_addr, buffer, INET6_ADDRSTRLEN);
			lftpd_log_info("connection received from %s...", buffer);
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
	lftpd_start(cwd, 2121);
	free(cwd);
}

