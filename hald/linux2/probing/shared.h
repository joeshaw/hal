
#ifndef SHARED_H
#define SHARED_H

#include <time.h>
#include <sys/time.h>


static int priority;
static const char *file;
static int line;
static const char *function;

static int is_enabled = 1;



/** Logging levels for HAL daemon
 */
enum {
	HAL_LOGPRI_TRACE = (1 << 0),   /**< function call sequences */
	HAL_LOGPRI_DEBUG = (1 << 1),   /**< debug statements in code */
	HAL_LOGPRI_INFO = (1 << 2),    /**< informational level */
	HAL_LOGPRI_WARNING = (1 << 3), /**< warnings */
	HAL_LOGPRI_ERROR = (1 << 4)    /**< error */
};

/** Setup logging entry
 *
 *  @param  priority            Logging priority, one of HAL_LOGPRI_*
 *  @param  file                Name of file where the log entry originated
 *  @param  line                Line number of file
 *  @param  function            Name of function
 */
static void
logger_setup (int _priority, const char *_file, int _line, const char *_function)
{
	priority = _priority;
	file = _file;
	line = _line;
	function = _function;
}

/** Emit logging entry
 *
 *  @param  format              Message format string, printf style
 *  @param  ...                 Parameters for message, printf style
 */
static void
logger_emit (const char *format, ...)
{
	va_list args;
	char buf[512];
	char *pri;
	char tbuf[256];
	struct timeval tnow;
	struct tm *tlocaltime;
	struct timezone tzone;

	if (!is_enabled)
		return;

	va_start (args, format);
	vsnprintf (buf, sizeof (buf), format, args);

	switch (priority) {
	case HAL_LOGPRI_TRACE:
		pri = "[T]";
		break;
	case HAL_LOGPRI_DEBUG:
		pri = "[D]";
		break;
	case HAL_LOGPRI_INFO:
		pri = "[I]";
		break;
	case HAL_LOGPRI_WARNING:
		pri = "[W]";
		break;
	default:		/* explicit fallthrough */
	case HAL_LOGPRI_ERROR:
		pri = "[E]";
		break;
	}

	gettimeofday (&tnow, &tzone);
	tlocaltime = localtime (&tnow.tv_sec);
	strftime (tbuf, sizeof (tbuf), "%H:%M:%S", tlocaltime);

	/** @todo Make programmatic interface to logging */
	if (priority != HAL_LOGPRI_TRACE)
		fprintf (stderr, "%s.%03d %s %s:%d: %s\n", tbuf, (int)(tnow.tv_usec/1000), pri, file, line, buf);

	va_end (args);
}


/** Trace logging macro */
#define HAL_TRACE(expr)   do {logger_setup(HAL_LOGPRI_TRACE,   __FILE__, __LINE__, __FUNCTION__); logger_emit expr; } while(0)

/** Debug information logging macro */
#define HAL_DEBUG(expr)   do {logger_setup(HAL_LOGPRI_DEBUG,   __FILE__, __LINE__, __FUNCTION__); logger_emit expr; } while(0)

/** Information level logging macro */
#define HAL_INFO(expr)    do {logger_setup(HAL_LOGPRI_INFO,    __FILE__, __LINE__, __FUNCTION__); logger_emit expr; } while(0)

/** Warning level logging macro */
#define HAL_WARNING(expr) do {logger_setup(HAL_LOGPRI_WARNING, __FILE__, __LINE__, __FUNCTION__); logger_emit expr; } while(0)

/** Error leve logging macro */
#define HAL_ERROR(expr)   do {logger_setup(HAL_LOGPRI_ERROR,   __FILE__, __LINE__, __FUNCTION__); logger_emit expr; } while(0)


#endif
