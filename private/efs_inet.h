#pragma once

int efs_inet_listen(int port);
int efs_inet_get_socket_port(int socket);

/**
 * @brief Read a line from the client, terminating when CRLF is received
 * or the buffer length is reached.
 */
int efs_inet_read_line(int socket, char* buffer, size_t buffer_len);

int efs_inet_writef(int socket, char* buffer, size_t buffer_len, char* format, ...);
