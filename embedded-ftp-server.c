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

#define CRLF "\r\n"

typedef enum {
	EPSV,
	FEAT,
	LIST,
	PASS,
	PASV,
	PWD,
	QUIT,
	SYST,
	USER,
} command_t;

typedef struct {
	int socket;
} client_context_t;

static void log_internal(const char* level, const char* format, ...) {
	char message[1024];
	va_list args;
	va_start(args, format);
	vsprintf(message, format, args);
	va_end(args);

	printf("%s %s\n", level, message);
}

#define log_error(format, ...) log_internal("ERROR", format, ##__VA_ARGS__)
#define log_info(format, ...) log_internal("INFO", format, ##__VA_ARGS__)

/**
 * @brief Write a NULL terminated string to the client. This is simply
 * a wrapper around write() which calls write() as many times as needed
 * to write the entire string.
 */
static int write_string(client_context_t* context, char* s) {
	char* p = s;
	int length = strlen(s);
	while (length) {
		int write_len = write(context->socket, p, length);
		if (write_len < 0) {
			log_error("write error");
			return write_len;
		}
		p += write_len;
		length -= write_len;
	}
	log_info("> %s", s);
	return 0;
}

/**
 * @brief Write a simple response of the form NNN ssss...\r\n to the client.
 * For example, write_simple_response(context, 220, "Welcome") will write
 * "220 Welcome\r\n" to the client.
 */
static int write_simple_response(client_context_t* context, int code, const char* message) {
	char buffer[256];
	int length = snprintf(buffer, sizeof(buffer), "%d %s%s", code, message, CRLF);
	if (length >= sizeof(buffer)) {
		log_error("message too long");
		return -1;
	}
	return write_string(context, buffer);
}

static int write_continue_response(client_context_t* context, int code, const char* message) {
	char buffer[256];
	int length = snprintf(buffer, sizeof(buffer), "%d-%s%s", code, message, CRLF);
	if (length >= sizeof(buffer)) {
		log_error("message too long");
		return -1;
	}
	return write_string(context, buffer);
}

/**
 * @brief Read a line from the client, terminating when CRLF is received
 * or the buffer length is reached.
 */
static int read_line(client_context_t* context, char* buffer, int length) {
	memset(buffer, 0, length);
	int total_read_len = 0;
	while (total_read_len < length) {
		// read up to length - 1 bytes. the - 1 leaves room for the
		// null terminator.
		int read_len = read(context->socket,
				buffer + total_read_len,
				length - total_read_len - 1);
		if (read_len == 0) {
			// end of stream - since we didn't find the end of line in
			// the previous pass we won't find it in this one, so this
			// is an error.
			return -1;
		}
		else if (read_len < 0) {
			// general error
			return read_len;
		}
		total_read_len += read_len;
		char* p = strstr(buffer, CRLF);
		if (p) {
			// null terminate the line and return
			*p = '\0';
			log_info("< '%s'", buffer);
			return 0;
		}
	}
	return -1;
}

static int parse_command(client_context_t* context, char* command_str, command_t* command, char** argument) {
	// find the index of the first space
	int index;
	char* p = strchr(command_str, ' ');
	if (p != NULL) {
		index = p - command_str;
	}
	// if no space, use the whole string
	else {
		index = strlen(command_str);
	}

	// if the index is 5 or greater the command is too long
	if (index >= 5) {
		log_error("bad command code, too long (%d)", index);
		return -1;
	}

	// duplicate the command
	char command_tmp[4 + 1];
	memset(command_tmp, 0, sizeof(command_tmp));
	memcpy(command_tmp, command_str, index);

	// upper case it
	for (int i = 0; command_tmp[i]; i++) {
		command_tmp[i] = toupper(command_tmp[i]);
	}

//	EPSV,
//	FEAT,
//	PASS,
//	PASV,
//	PWD,
//	QUIT,
//	SYST,
//	USER,

	printf("command '%s'\n", command_tmp);

	// compare tree
	if (strcmp("EPSV", command_tmp) == 0) {
		*command = EPSV;
	}
	else if (strcmp("FEAT", command_tmp) == 0) {
		*command = FEAT;
	}
	else if (strcmp("LIST", command_tmp) == 0) {
		*command = LIST;
	}
	else if (strcmp("PASS", command_tmp) == 0) {
		*command = PASS;
	}
	else if (strcmp("PASV", command_tmp) == 0) {
		*command = PASV;
	}
	else if (strcmp("PWD", command_tmp) == 0) {
		*command = PWD;
	}
	else if (strcmp("QUIT", command_tmp) == 0) {
		*command = QUIT;
	}
	else if (strcmp("SYST", command_tmp) == 0) {
		*command = SYST;
	}
	else if (strcmp("USER", command_tmp) == 0) {
		*command = USER;
	}
	else {
		return -1;
	}

	// find the next non-space character for the argument
	// TODO when we need arguments

	return 0;
}

static int open_data_port_socket(client_context_t* context) {
	// DRY this out?
	int server_socket = socket(AF_INET6, SOCK_STREAM, 0);

	if (server_socket < 0) {
	  log_error("error creating data port socket");
	  return -1;
	}

	struct sockaddr_in6 server_addr = {
		   .sin6_family = AF_INET6,
		   .sin6_addr = in6addr_any,
		   .sin6_port = 0,
	};

	int err = bind(server_socket, (struct sockaddr *) &server_addr, sizeof(server_addr));
	if (err < 0) {
	  log_error("error binding data port");
	  return -1;
	}

	err = listen(server_socket, 10);
	if (err < 0) {
	   log_error("error creating data port listener");
	   return -1;
	}

	return server_socket;
}

static int handle_control_channel(client_context_t* context) {
	int err = write_simple_response(context, 220, "Welcome to embedded-ftp-server (v1.0).");
	if (err != 0) {
		log_error("error sending welcome message");
		goto cleanup;
	}

	char read_buffer[256];
	while (err == 0) {
		int line_len = read_line(context, read_buffer, sizeof(read_buffer));
		if (line_len != 0) {
			log_error("error reading next command");
			goto cleanup;
		}
		command_t command = QUIT;
		char* argument = NULL;
		err = parse_command(context, read_buffer, &command, &argument);
		if (err != 0) {
			log_error("error parsing command");
			goto cleanup;
		}

		switch (command) {
			case EPSV: {
				// open a data port
				int data_port_socket = open_data_port_socket(context);
				if (data_port_socket < 0) {
					// TODO find proper result code
					write_simple_response(context, 500, "Error: Unable to open data port.");
					break;
				}

				// get it's port number
				struct sockaddr_in6 data_port_addr;
				socklen_t data_port_addr_len = sizeof(struct sockaddr_in6);
				err = getsockname(data_port_socket, (struct sockaddr*) &data_port_addr, &data_port_addr_len);
				if (err != 0) {
					log_error("error getting data port port number");
					return err;
				}
				int port = ntohs(data_port_addr.sin6_port);

				// format the response
				char message[256];
				snprintf(message, sizeof(message), "Entering Extended Passive Mode (|||%d|).", port);
				write_simple_response(context, 229, message);

				// wait for the connection to the data port
				// TODO think about handling multiple, and whether these
				// go into the context
				log_info("waiting for data port connection...");
				int client_socket = accept(data_port_socket, NULL, NULL);
				if (client_socket < 0) {
				  log_error("error accepting client socket");
				  continue;
				}
				log_info("data port connection received...");

				break;
			}
			case FEAT:
				write_continue_response(context, 211, "Features:");
				write_string(context, "EPSV\r\n");
				write_string(context, "PASV\r\n");
				write_string(context, "SIZE\r\n");
				write_simple_response(context, 211, "End");
				break;
			case LIST:
				// TODO
				write_simple_response(context, 150, "Here comes the directory listing.");
//				write_string(context, "-rw-r--r--    1 0        0        1073741824000 Feb 19  2016 1000GB.zip\r\n");
				write_simple_response(context, 226, "Directory send OK.");
				break;
			case PASS:
				write_simple_response(context, 230, "Login successful.");
				break;
			case PASV:
				break;
			case PWD:
				write_simple_response(context, 257, "\"/\"");
				break;
			case QUIT:
				write_simple_response(context, 221, "Bye.");
				break;
			case SYST:
				write_simple_response(context, 215, "UNIX Type: L8");
				break;
			case USER:
				write_simple_response(context, 230, "Login successful.");
				break;
		}
	}

	cleanup:
	close(context->socket);

	return 0;
}

int ftp_server_start(const char* directory, int port) {
   int server_socket = socket(AF_INET6, SOCK_STREAM, 0);

   if (server_socket < 0) {
	  log_error("error creating server socket");
	  return -1;
   }

   struct sockaddr_in6 server_addr = {
		   .sin6_family = AF_INET6,
		   .sin6_addr = in6addr_any,
		   .sin6_port = htons(port),
   };

   int err = bind(server_socket, (struct sockaddr *) &server_addr, sizeof(server_addr));
   if (err < 0) {
	  log_error("error binding to port %d", port);
	  return -1;
   }

   err = listen(server_socket, 10);
   if (err < 0) {
	   log_error("error creating listener");
	   return -1;
   }

   while (true) {
	   log_info("waiting for connection...");

	   int client_socket = accept(server_socket, NULL, NULL);
	   if (client_socket < 0) {
		  log_error("error accepting client socket");
		  continue;
	   }

	   log_info("connection received...");
	   client_context_t context = {
			   .socket = client_socket
	   };
	   handle_control_channel(&context);
   }

   return 0;
}

int main( int argc, char *argv[] ) {
	char* cwd = getcwd(NULL, 0);
	ftp_server_start(cwd, 2121);
	free(cwd);
}



/*

https://tools.ietf.org/html/rfc959
https://tools.ietf.org/html/rfc2389#section-2.2
https://tools.ietf.org/html/rfc3659
https://tools.ietf.org/html/rfc5797
https://tools.ietf.org/html/rfc2428#section-3 EPSV

<connect>
> 220 Welcome to ftpd (v2.3).
< USER anonymous
> 331 Please specify the password.
< PASS anonymous
> 230 Login successful.
< SYST
> 215 UNIX Type: L8
< FEAT
> 211-Features:
 EPRT
 EPSV
 MDTM
 PASV
 REST STREAM
 SIZE
 TVFS
 UTF8
211 End
< PWD
> 257 "/"
< EPSV
> 229 Entering Extended Passive Mode (|||2550|).
< LIST
> 150 Here comes the directory listing.
-rw-r--r--    1 0        0        1073741824000 Feb 19  2016 1000GB.zip
-rw-r--r--    1 0        0        107374182400 Feb 19  2016 100GB.zip
-rw-r--r--    1 0        0          102400 Feb 19  2016 100KB.zip
226 Directory send OK.

> ftp -d -v -a speedtest.tele2.net
Trying 90.130.70.73...
Connected to speedtest.tele2.net.
220 (vsFTPd 2.3.5)
ftp_login: user `<null>' pass `<null>' host `speedtest.tele2.net'
---> USER anonymous
331 Please specify the password.
---> PASS XXXX
230 Login successful.
---> SYST
215 UNIX Type: L8
Remote system type is UNIX.
Using binary mode to transfer files.
---> FEAT
211-Features:
 EPRT
 EPSV
 MDTM
 PASV
 REST STREAM
 SIZE
 TVFS
 UTF8
211 End
features[FEAT_FEAT] = 1
features[FEAT_MDTM] = 1
features[FEAT_MLST] = 0
features[FEAT_REST_STREAM] = 1
features[FEAT_SIZE] = 1
features[FEAT_TVFS] = 1
got localcwd as `/Users/jason'
---> PWD
257 "/"
got remotecwd as `/'
ftp> ls
---> EPSV
229 Entering Extended Passive Mode (|||25530|).
229 Entering Extended Passive Mode (|||25530|).
---> LIST
150 Here comes the directory listing.
-rw-r--r--    1 0        0        1073741824000 Feb 19  2016 1000GB.zip
-rw-r--r--    1 0        0        107374182400 Feb 19  2016 100GB.zip
-rw-r--r--    1 0        0          102400 Feb 19  2016 100KB.zip
-rw-r--r--    1 0        0        104857600 Feb 19  2016 100MB.zip
-rw-r--r--    1 0        0        10737418240 Feb 19  2016 10GB.zip
-rw-r--r--    1 0        0        10485760 Feb 19  2016 10MB.zip
-rw-r--r--    1 0        0        1073741824 Feb 19  2016 1GB.zip
-rw-r--r--    1 0        0            1024 Feb 19  2016 1KB.zip
-rw-r--r--    1 0        0         1048576 Feb 19  2016 1MB.zip
-rw-r--r--    1 0        0        209715200 Feb 19  2016 200MB.zip
-rw-r--r--    1 0        0        20971520 Feb 19  2016 20MB.zip
-rw-r--r--    1 0        0         2097152 Feb 19  2016 2MB.zip
-rw-r--r--    1 0        0         3145728 Feb 19  2016 3MB.zip
-rw-r--r--    1 0        0        524288000 Feb 19  2016 500MB.zip
-rw-r--r--    1 0        0        52428800 Feb 19  2016 50MB.zip
-rw-r--r--    1 0        0          524288 Feb 19  2016 512KB.zip
-rw-r--r--    1 0        0         5242880 Feb 19  2016 5MB.zip
drwxr-xr-x    2 105      108         12288 Jul 04 19:25 upload
226 Directory send OK.
ftp> get 2MB.zip
local: 2MB.zip remote: 2MB.zip
---> TYPE I
200 Switching to Binary mode.
---> SIZE 2MB.zip
213 2097152
---> EPSV
229 Entering Extended Passive Mode (|||22012|).
229 Entering Extended Passive Mode (|||22012|).
---> RETR 2MB.zip
150 Opening BINARY mode data connection for 2MB.zip (2097152 bytes).
100% |******************************************************************************************************************************************************************************************************************|  2048 KiB  857.46 KiB/s    00:00 ETA226 Transfer complete.
2097152 bytes received in 00:02 (818.52 KiB/s)
---> MDTM 2MB.zip
213 20160219174152
parsed date `20160219174152' as 1455903712, Fri, 19 Feb 2016 11:41:52 -0600
ftp>




 */
