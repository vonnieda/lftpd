#pragma once

#define lftpd_log_error(format, ...) lftpd_log_internal("ERROR", format, ##__VA_ARGS__)
#define lftpd_log_info(format, ...) lftpd_log_internal("INFO", format, ##__VA_ARGS__)

void lftpd_log_internal(const char* level, const char* format, ...);

