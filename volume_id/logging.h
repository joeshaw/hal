/*
 * volume_id_logging - this file is used to map the dbg() function
 *                     to the user's logging facility
 *
 */

#ifndef _VOLUME_ID_LOGGING_H_
#define _VOLUME_ID_LOGGING_H_

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef DEBUG
#define dbg(format, arg...)							\
	do {									\
		volume_id_log("%s: " format "\n", __FUNCTION__ , ## arg);	\
	} while (0)
#else
#define dbg(format, arg...)	do {} while (0)
#endif

#endif /* _VOLUME_ID_LOGGING_H_ */
