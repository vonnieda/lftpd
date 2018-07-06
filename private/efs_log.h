#pragma once

#define efs_log_error(format, ...) efs_log_internal("ERROR", format, ##__VA_ARGS__)
#define efs_log_info(format, ...) efs_log_internal("INFO", format, ##__VA_ARGS__)

void efs_log_internal(const char* level, const char* format, ...);

