/***************************************************************************
 * CVSID: $Id$
 *
 * logger.h : Logging facility
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 *
 * Licensed under the Academic Free License version 2.0
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 **************************************************************************/

#ifndef LOGGER_H
#define LOGGER_H

/**
 * @addtogroup HalDaemonLogging
 *
 * @{
 */


/** Logging levels for HAL daemon
 */
enum
{
    HAL_LOGPRI_TRACE   =  (1<<0),  /**< function call sequences */
    HAL_LOGPRI_DEBUG   =  (1<<1),  /**< debug statements in code */
    HAL_LOGPRI_INFO    =  (1<<2),  /**< informational level */
    HAL_LOGPRI_WARNING =  (1<<3),  /**< warnings */
    HAL_LOGPRI_ERROR   =  (1<<4)   /**< error */
};

void logger_init();

void logger_setup(int priority, const char* file, int line, 
                  const char* function);

void logger_emit(const char* format,...);

#ifdef ENABLE_VERBOSE_MODE

/* Verbose mode */

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

/** Macro for terminating the program on an unrecoverable error */
#define DIE(expr) do {printf("*** [DIE] %s:%s():%d : ", __FILE__, __FUNCTION__, __LINE__); printf expr; printf("\n"); exit(1); } while(0)

#else

/* If verbose mode is not requested only handle warning and error */

#define HAL_TRACE(expr) do {;} while(0)
#define HAL_DEBUG(expr) do {;} while(0)
#define HAL_INFO(expr) do {;} while(0)

#define HAL_WARNING(expr) do {logger_setup(HAL_LOGPRI_WARNING, __FILE__, __LINE__, __FUNCTION__); logger_emit expr; } while(0)

#define HAL_ERROR(expr)   do {logger_setup(HAL_LOGPRI_ERROR,   __FILE__, __LINE__, __FUNCTION__); logger_emit expr; } while(0)

#define DIE(expr) do {printf("*** [DIE] %s:%s():%d : ", __FILE__, __FUNCTION__, __LINE__); printf expr; printf("\n"); exit(1); } while(0)


#endif /* ENABLE_VERBOSE_MODE */

/** @} */

#endif /* LOGGER_H */
