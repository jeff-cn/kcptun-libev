#ifndef SLOG_H
#define SLOG_H

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <time.h>

enum {
	LOG_LEVEL_SILENCE,
	LOG_LEVEL_FATAL,
	LOG_LEVEL_ERROR,
	LOG_LEVEL_WARNING,
	LOG_LEVEL_INFO,
	LOG_LEVEL_DEBUG,
	LOG_LEVEL_VERBOSE,
};

#define LOG_LEVEL_FATAL_STR "F"
#define LOG_LEVEL_ERROR_STR "E"
#define LOG_LEVEL_WARNING_STR "W"
#define LOG_LEVEL_INFO_STR "I"
#define LOG_LEVEL_DEBUG_STR "D"
#define LOG_LEVEL_VERBOSE_STR "V"

extern int slog_level;
extern FILE *slog_file;

#if defined(_MSC_VER)
#define PATH_SEPARATOR '\\'
#else
#define PATH_SEPARATOR '/'
#endif

#define LOGLEVEL(x) ((x) <= slog_level)

#define LOG_INTERNAL(level, path, line, format, ...)                           \
	do {                                                                   \
		if (LOGLEVEL(level)) {                                         \
			FILE *log_fp = slog_file ? slog_file : stdout;         \
			const time_t log_now = time(NULL);                     \
			char log_timestamp[32];                                \
			const int timestamp_len = strftime(                    \
				log_timestamp, sizeof(log_timestamp),          \
				"%FT%T%z", localtime(&log_now));               \
			const char *log_filename =                             \
				strrchr((path), PATH_SEPARATOR);               \
			if (log_filename && *log_filename) {                   \
				log_filename++;                                \
			} else {                                               \
				log_filename = (path);                         \
			}                                                      \
			(void)fprintf(                                         \
				log_fp, level##_STR " %*s %s:%d " format "\n", \
				timestamp_len, log_timestamp, log_filename,    \
				(line), __VA_ARGS__);                          \
			(void)fflush(log_fp);                                  \
		}                                                              \
	} while (0)

/* Fatal: Log an fatal message. */
#define LOGF_F(format, ...)                                                    \
	LOG_INTERNAL(LOG_LEVEL_FATAL, __FILE__, __LINE__, format, __VA_ARGS__)
#define LOGF(message) LOGF_F("%s", message)

/* Error: Log an error message. */
#define LOGE_F(format, ...)                                                    \
	LOG_INTERNAL(LOG_LEVEL_ERROR, __FILE__, __LINE__, format, __VA_ARGS__)
#define LOGE(message) LOGE_F("%s", message)

/* Warning: Log a warning message. */
#define LOGW_F(format, ...)                                                    \
	LOG_INTERNAL(LOG_LEVEL_WARNING, __FILE__, __LINE__, format, __VA_ARGS__)
#define LOGW(message) LOGW_F("%s", message)

/* Info: Log an info message. */
#define LOGI_F(format, ...)                                                    \
	LOG_INTERNAL(LOG_LEVEL_INFO, __FILE__, __LINE__, format, __VA_ARGS__)
#define LOGI(message) LOGI_F("%s", message)

/* Debug: Log a debug message. */
#define LOGD_F(format, ...)                                                    \
	LOG_INTERNAL(LOG_LEVEL_DEBUG, __FILE__, __LINE__, format, __VA_ARGS__)
#define LOGD(message) LOGD_F("%s", message)

/* Verbose: Log a verbose message. */
#define LOGV_F(format, ...)                                                    \
	LOG_INTERNAL(LOG_LEVEL_VERBOSE, __FILE__, __LINE__, format, __VA_ARGS__)
#define LOGV(message) LOGV_F("%s", message)

/* perror: Log last system error message. */
#define LOG_PERROR(level, message)                                             \
	do {                                                                   \
		const int err = errno;                                         \
		LOG_INTERNAL(                                                  \
			level, __FILE__, __LINE__, "%s: [%d] %s", message,     \
			err, strerror(err));                                   \
	} while (0)

#define LOGF_PERROR(message) LOG_PERROR(LOG_LEVEL_FATAL, message)
#define LOGE_PERROR(message) LOG_PERROR(LOG_LEVEL_ERROR, message)
#define LOGW_PERROR(message) LOG_PERROR(LOG_LEVEL_WARNING, message)
#define LOGI_PERROR(message) LOG_PERROR(LOG_LEVEL_INFO, message)
#define LOGD_PERROR(message) LOG_PERROR(LOG_LEVEL_DEBUG, message)
#define LOGV_PERROR(message) LOG_PERROR(LOG_LEVEL_VERBOSE, message)

#endif /* SLOG_H */
