/***************************************************************************
 * CVSID: $Id$
 *
 * mmap_cache.c : FDI cache routines.
 *
 * Copyright (C) 2003 David Zeuthen, <david@fubar.dk>
 * Copyright (C) 2006 Kay Sievers, <kay.sievers@vrfy.org>
 * Copyright (C) 2006 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2007 Mikhail Kshevetskiy <mikhail.kshevetskiy@gmail.com>
 * Copyright (C) 2007 Sergey Lapin <slapinid@gmail.com>
 *
 * Licensed under the Academic Free License version 2.1
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#include <config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>

#include "logger.h"
#include "rule.h"
#include "mmap_cache.h"
#include "hald_runner.h"

extern void *rules_ptr;
static int rules_fd = -1;
size_t rules_size = 0;

void di_rules_init(void)
{
	struct cache_header	*header;
	char 			*cachename;
	int			fd;
	struct stat		statbuf;

	cachename = getenv ("HAL_FDI_CACHE_NAME");
	if(cachename == NULL) 
		cachename = HALD_CACHE_FILE;

	if (rules_ptr != NULL) {
		HAL_INFO (("Unmapping old cache file"));
		munmap (rules_ptr, rules_size);
	}

	if((fd = open (cachename, O_RDONLY)) < 0)
		DIE(("Unable to open cache %s\n", cachename));

	if(fstat (fd,&statbuf) < 0)
		DIE(("Unable to stat cache %s\n", cachename));

	rules_size = statbuf.st_size;

	rules_ptr = mmap (NULL, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if(rules_ptr == MAP_FAILED)
		DIE (("Couldn't mmap file '%s', errno=%d: %s", cachename, errno, strerror (errno)));

	header = (struct cache_header*) rules_ptr;
	HAL_INFO(("preprobe: offset=%08lx, size=%d", header->fdi_rules_preprobe,
		header->fdi_rules_information - header->fdi_rules_preprobe));
	HAL_INFO(("information: offset=%08lx, size=%d", header->fdi_rules_information,
		header->fdi_rules_policy - header->fdi_rules_information));
	HAL_INFO(("policy: offset=%08lx, size=%d", header->fdi_rules_policy,
		header->all_rules_size - header->fdi_rules_policy));

	close(fd);
}

static void dir_mtime(const char * path, time_t * mt)
{
	struct dirent **namelist;
	struct stat st;
	int n;
	char cpath[PATH_MAX];
	
	if (!stat(path, &st)) {
		if(st.st_mtime > *mt)
			*mt = st.st_mtime;
		
		if(S_ISDIR(st.st_mode)) {
			n = scandir(path, &namelist, 0, alphasort);
			if (n < 0)
				return;
			else {
				while(n--) {
#ifdef HAVE_SNPRINTF
					snprintf(cpath, PATH_MAX, "%s/%s", path, namelist[n]->d_name);
#else
					sprintf(cpath, "%s/%s", path, namelist[n]->d_name);
#endif
					if(namelist[n]->d_name[0] != '.')
						dir_mtime(cpath, mt);
					
					free(namelist[n]);
				}
				free(namelist);
			}
		}
	}
}


static gboolean regen_cache_done;
static gint regen_cache_success;

static void 
regen_cache_cb (HalDevice *d, 
		guint32 exit_type, 
		gint return_code, 
		gchar **error,
		gpointer data1, 
		gpointer data2)
{
	HAL_INFO (("In regen_cache_cb exit_type=%d, return_code=%d", exit_type, return_code));

	if (exit_type != HALD_RUN_SUCCESS || return_code != 0) {
		regen_cache_success = FALSE;
	} else {
		regen_cache_success = TRUE;
	}

	regen_cache_done = TRUE;
}


static void 
regen_cache (void)
{
	int n, m;
	char *extra_env[5] = {NULL, NULL, NULL, NULL, NULL};
	char *env_names[5] = {"HAL_FDI_SOURCE_PREPROBE", 
			      "HAL_FDI_SOURCE_INFORMATION", 
			      "HAL_FDI_SOURCE_POLICY",
			      "HAL_FDI_CACHE_NAME",
			      NULL};

	HAL_INFO (("Regenerating fdi cache.."));

	/* pass these variables to the helper */
	for (n = 0, m = 0; env_names[n] != NULL; n++) {
		char *name;
		char *val;

		name = env_names[n];
		val = getenv(name);
		if (val != NULL) {
			extra_env [m++] = g_strdup_printf ("%s=%s", name, val);
		}
	}

	regen_cache_done = FALSE;

	hald_runner_run (NULL, 
			 "hald-generate-fdi-cache --force",
			 extra_env,
			 10000,
			 regen_cache_cb,
			 NULL,
			 NULL);

	for (n = 0; extra_env[n] != NULL; n++) {
		g_free (extra_env[n]);
	}

	while (!regen_cache_done) {
		g_main_context_iteration (NULL, TRUE);
	}

	if (!regen_cache_success) {
		DIE (("fdi cache regeneration failed!"));
	}

	HAL_INFO (("fdi cache generation done"));
}

gboolean
di_cache_coherency_check (void)
{
	char *hal_fdi_source_preprobe;
	char *hal_fdi_source_information;
	char *hal_fdi_source_policy;
	char *cachename;
	time_t mt;
	struct stat st;
	gboolean did_regen;

	did_regen = FALSE;

	mt = 0;

	hal_fdi_source_preprobe = getenv ("HAL_FDI_SOURCE_PREPROBE");
	hal_fdi_source_information = getenv ("HAL_FDI_SOURCE_INFORMATION");
	hal_fdi_source_policy = getenv ("HAL_FDI_SOURCE_POLICY");

	if (hal_fdi_source_preprobe != NULL) {
		dir_mtime (hal_fdi_source_preprobe, &mt);
	} else {
		dir_mtime (PACKAGE_DATA_DIR "/hal/fdi/preprobe", &mt);
		dir_mtime (PACKAGE_SYSCONF_DIR "/hal/fdi/preprobe", &mt);
	}

	if (hal_fdi_source_information != NULL) {
		dir_mtime (hal_fdi_source_information, &mt);
	} else {
		dir_mtime (PACKAGE_DATA_DIR "/hal/fdi/information", &mt);
		dir_mtime (PACKAGE_SYSCONF_DIR "/hal/fdi/information", &mt);
	}

	if (hal_fdi_source_policy != NULL) {
		dir_mtime (hal_fdi_source_policy, &mt);
	} else {
		dir_mtime (PACKAGE_DATA_DIR "/hal/fdi/policy", &mt);
		dir_mtime (PACKAGE_SYSCONF_DIR "/hal/fdi/policy", &mt);
	}

	cachename = getenv ("HAL_FDI_CACHE_NAME");
	if(cachename == NULL)
		cachename = HALD_CACHE_FILE;

	if(stat (cachename, &st) == 0) {
		if(st.st_mtime < mt) {
			HAL_INFO(("Cache needs update"));
			regen_cache();
			did_regen = TRUE;
		}
	} else {
		regen_cache();
		did_regen = TRUE;
	}
	
	HAL_INFO(("cache mtime is %d",mt));

	return did_regen;
}
