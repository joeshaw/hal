/*
 * volume_id_logging - this file is used to map the dbg() function
 *                     to the host logging facility
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

#include "../../logger.h"
#define dbg(s...)	HAL_INFO((s))

#endif /* _VOLUME_ID_LOGGING_H_ */
