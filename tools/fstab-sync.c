/* Copyright 2004 Red Hat, Inc.
 *
 * This software may be freely redistributed under the terms of the GNU
 * Lesser General Public license.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* This program serves one major purpose:
 *    - Update the fs table in response to HAL events
 *    
 * Additionally, this program offers another option of removing
 * any trace of its previous actions from the fs table. Specifically
 * this happens when invoked by the HAL daemon for the root computer
 * object which can only happen when the HAL daemon is starting up.
 * Thus, when starting the HAL daemon, the /etc/fstab file will be
 * completely sanitized.
 *
 * Because it is possible that this program could be invoked multiple
 * times (at bootup or when a user plugs in multiple pieces of
 * hardware), it is very important that all the different instances of
 * this program do not step on each others toes.  Also, it is important
 * that this program does not corrupt the fs table in the event of
 * error conditions like out-of-disk-space and power outages.  
 *
 * For these reasons, all operations are done on temporary copies of
 * /etc/fstab in /etc itself. After any particular instance is done it
 * copies its temporary file over to /etc/fstab; this is guaranteed to
 * be atomic. 
 *
 * TODO: add support for using /tmp if /etc is mounted readonly (with 
 * /etc/fstab being on a tmpfs or ramfs)
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fstab.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#include <mntent.h>
#include <syslog.h>

#include <popt.h>


#define DBUS_API_SUBJECT_TO_CHANGE
#include "../libhal/libhal.h"
#include "../libhal-storage/libhal-storage.h"

typedef int boolean;

static boolean verbose = FALSE;

#define _(a) (a)
#define N_(a) a

#define PROGRAM_NAME "fstab-sync"
#define TEMP_FSTAB_PREFIX ".fstab.hal."
#define TEMP_FSTAB_MAX_LENGTH 64

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE !TRUE
#endif

#define fstab_update_debug(...) do {if (verbose) fprintf (stderr, __VA_ARGS__);} while (0)

static pid_t pid;

static char   *fsy_mount_root;
static boolean fsy_use_managed;
static char   *fsy_managed_primary;
static char   *fsy_managed_secondary;

typedef enum 
{
  FS_TABLE_FIELD_TYPE_BLOCK_DEVICE = 0,
  FS_TABLE_FIELD_TYPE_MOUNT_POINT,
  FS_TABLE_FIELD_TYPE_FILE_SYSTEM_TYPE,
  FS_TABLE_FIELD_TYPE_MOUNT_OPTIONS,
  FS_TABLE_FIELD_TYPE_DUMP_FREQUENCY,
  FS_TABLE_FIELD_TYPE_PASS_NUMBER,
  FS_TABLE_FIELD_TYPE_WHITE_SPACE,
  FS_TABLE_NUM_FIELD_TYPES
} FSTableFieldType;

typedef struct FSTableField
{
  FSTableFieldType type;
  char *value;
  struct FSTableField *next;
} FSTableField;

typedef struct FSTableLine
{
  FSTableField *fields, *tail;

  char *block_device;
  char *device_type;
  char *fs_type;
  char *mount_point;
  char *mount_options;
  char *dump_frequency;
  char *pass_number;

  struct FSTableLine *next;
} FSTableLine;

typedef struct
{
  FSTableLine *lines, *tail;
  char *parse_buffer;
  size_t parse_buffer_length;
  size_t parse_buffer_capacity;
} FSTable;

static LibHalContext *hal_context = NULL;

static void    fs_table_line_add_field      (FSTableLine *line, FSTableField *field);
static boolean fs_table_line_is_generated   (FSTableLine *line);
static void    fs_table_line_update_pointer (FSTableLine *line, FSTableField *field);

static FSTable *fs_table_new        (const char *filename);
static void     fs_table_free       (FSTable *table);
static boolean  fs_table_parse_data (FSTable *table, const char *data, size_t length);


static inline int get_random_int_in_range (int low, int high);
static int        open_temp_fstab_file (const char *dir, char **filename);
static boolean    create_mount_point_for_volume (const char *full_mount_path);
static boolean    fs_table_line_has_mount_option (FSTableLine *line, const char *option);

/*static boolean      fs_table_add_volume    (FSTable *table, const char *udi);*/
static FSTableLine *fs_table_remove_volume (FSTable *table, const char *block_device);

static boolean add_udi (const char *udi);
static boolean remove_udi (const char *udi);
static boolean clean (void);


static FSTableField *
fs_table_field_new (FSTableFieldType type, const char *value)
{
  FSTableField *field;

  field = malloc (sizeof (FSTableLine));

  field->type = type;
  field->value = strdup (value);
  field->next = NULL;

  return field;
}

static void
fs_table_field_free (FSTableField *field)
{

  if (field == NULL)
    return;

  field->type = FS_TABLE_FIELD_TYPE_WHITE_SPACE;

  if (field->value != NULL)
    {
      free (field->value);
      field->value = NULL;
    }
}

static FSTableLine *
fs_table_line_new (void)
{
  FSTableLine *line;

  line = calloc (sizeof (FSTableLine), 1);

  return line;
}

static const char *
get_whitespace (const char *field, int preferred_width)
{
  static const char whitespace[] = "                                           ";
  static const int whitespace_length = (const int) sizeof (whitespace);
  int field_length;

  field_length = strlen (field);

  if (field_length >= preferred_width)
    return " ";

  if (preferred_width > whitespace_length)
    preferred_width = whitespace_length;

  return whitespace + whitespace_length - preferred_width + field_length - 1;
}

static FSTableLine *
fs_table_line_new_from_field_values (const char *block_device,
                                     const char *mount_point,
                                     const char *fs_type,
                                     const char *mount_options,
                                     int dump_frequency,
                                     int pass_number)
{
  FSTableLine *line;
  FSTableField *field;
  char number[32];

  line = fs_table_line_new ();

  field = fs_table_field_new (FS_TABLE_FIELD_TYPE_BLOCK_DEVICE, block_device); 
  fs_table_line_add_field (line, field);

  field = fs_table_field_new (FS_TABLE_FIELD_TYPE_WHITE_SPACE, 
                              get_whitespace (block_device, 24)); 
  fs_table_line_add_field (line, field);

  field = fs_table_field_new (FS_TABLE_FIELD_TYPE_MOUNT_POINT, mount_point); 
  fs_table_line_add_field (line, field);
  
  field = fs_table_field_new (FS_TABLE_FIELD_TYPE_WHITE_SPACE, 
                              get_whitespace (mount_point, 24)); 
  fs_table_line_add_field (line, field);

  field = fs_table_field_new (FS_TABLE_FIELD_TYPE_FILE_SYSTEM_TYPE, fs_type); 
  fs_table_line_add_field (line, field);

  field = fs_table_field_new (FS_TABLE_FIELD_TYPE_WHITE_SPACE, 
                              get_whitespace (fs_type, 8)); 
  fs_table_line_add_field (line, field);

  field = fs_table_field_new (FS_TABLE_FIELD_TYPE_MOUNT_OPTIONS, mount_options);
  fs_table_line_add_field (line, field);

  field = fs_table_field_new (FS_TABLE_FIELD_TYPE_WHITE_SPACE, 
                              get_whitespace (mount_options, 16)); 
  fs_table_line_add_field (line, field);

  snprintf (number, 32, "%d", dump_frequency);
  field = fs_table_field_new (FS_TABLE_FIELD_TYPE_DUMP_FREQUENCY, number); 
  fs_table_line_add_field (line, field);

  field = fs_table_field_new (FS_TABLE_FIELD_TYPE_WHITE_SPACE, 
                              get_whitespace (number, 1)); 
  fs_table_line_add_field (line, field);

  snprintf (number, 32, "%d", pass_number);
  field = fs_table_field_new (FS_TABLE_FIELD_TYPE_PASS_NUMBER, number); 
  fs_table_line_add_field (line, field);

  return line;
}

static void
fs_table_line_add_field (FSTableLine *line, FSTableField *field)
{
  if (line->fields == NULL)
    {
      line->fields = field;
      line->tail = field;
    }
  else
    {
      line->tail->next = field;
      line->tail = field;
    }

  fs_table_line_update_pointer (line, field);
}

static boolean fs_table_line_is_generated (FSTableLine *line)
{
  boolean has_managed_keyword;

  has_managed_keyword = FALSE;

  if (fsy_use_managed) {
	  if (fs_table_line_has_mount_option (line, fsy_managed_primary))
		  has_managed_keyword = TRUE;
	  if (fs_table_line_has_mount_option (line, fsy_managed_secondary))
		  has_managed_keyword = TRUE;
  }

  if (!has_managed_keyword)
    return FALSE;

/*
  if (strncmp (line->mount_point, FSTAB_SYNC_MOUNT_ROOT "/", sizeof (FSTAB_SYNC_MOUNT_ROOT "/") -1) != 0)
    return FALSE;
*/

  return TRUE;
}

static void
fs_table_line_free (FSTableLine *line)
{
  FSTableField *field;

  if (line == NULL)
    return;

  field = line->fields;
  while (field != NULL)
    {
      FSTableField *this_field;

      this_field = field;
      field = field->next;

      fs_table_field_free (this_field);
    }

  line->fields = NULL;
  line->next = NULL;

  free (line);
}

static void
fs_table_add_line (FSTable *table, FSTableLine *line)
{
  if (table->lines == NULL)
    {
      table->lines = line;
      table->tail = line;
    }
  else
    {
      table->tail->next = line;
      table->tail = line;
    }
}

static char *
fs_table_to_string (FSTable *table, size_t *length)
{
  char *output_string;
  size_t output_string_length, output_string_capacity;
  FSTableLine *line;

  output_string_capacity = 1024;
  output_string = malloc (output_string_capacity);
  output_string[0] = '\0';
  output_string_length = 0;

  line = table->lines;
  while (line != NULL)
    {
      FSTableField *field;

      field = line->fields;
      while (field != NULL)
        {
          size_t field_length;

          field_length = strlen (field->value);

          if (output_string_length + field_length >= output_string_capacity - 1)
            {
              output_string_capacity *= 2;

              output_string = realloc (output_string, output_string_capacity);
            }

          strcpy (output_string + output_string_length, field->value);
          output_string_length += field_length; 

          field = field->next;
        }

      if (output_string_length + 1 >= output_string_capacity - 1)
        {
          output_string_capacity *= 2;

          output_string = realloc (output_string, output_string_capacity);
        }
      output_string[output_string_length++] = '\n';
      output_string[output_string_length] = '\0';

      line = line->next;
    }

  if (length)
    *length = output_string_length;

  return output_string;
}

static FSTable *
fs_table_new (const char *filename)
{
  FSTable *table;
  int input_fd;
  ssize_t bytes_read;
  unsigned char read_buf[1024];

  table = malloc (sizeof (FSTable));

  table->lines = NULL;
  table->parse_buffer_capacity = 128;
  table->parse_buffer = malloc (table->parse_buffer_capacity);
  table->parse_buffer[0] = '\0';
  table->parse_buffer_length = 0;

  input_fd = open (filename, O_RDONLY);

  if (input_fd < 0)
    goto error;

  while ((bytes_read = read (input_fd, read_buf, 1024)) != 0)
    {
      if (bytes_read < 0)
        {
          if (errno == EINTR)
            continue;

          fstab_update_debug (_("%d: Could not read from '%s': %s\n"),
                              pid, filename, strerror (errno));
          goto error;
        }

      if (!fs_table_parse_data (table, read_buf, (size_t) bytes_read))
        {
          fstab_update_debug (_("%d: Could not parse data from '%s'\n"), 
			      pid, filename);
          goto error;
        }
    }

  close (input_fd);

  return table;

error:

  fs_table_free (table);

  if (input_fd >= 0)
    close (input_fd);

  return NULL;
}

static boolean
fs_table_write (FSTable *table, int fd)
{
  char *fstab_contents;
  size_t bytes_written, bytes_to_write;

  fstab_contents = fs_table_to_string (table, &bytes_to_write);

  bytes_written = 0;
  while ((bytes_written = write (fd, fstab_contents + bytes_written, 
                                 bytes_to_write)) < bytes_to_write)
    {
      if (bytes_written < 0)
        {
          if (errno == EINTR)
            continue;

          fstab_update_debug (_("%d: Could not write to temporary file: %s\n"),
                              pid, strerror (errno));
          return FALSE;
        }

      bytes_to_write -= bytes_written;
    }

  free (fstab_contents);

  return TRUE;
}

static void
fs_table_free (FSTable *table)
{
  FSTableLine *line;

  line = table->lines;
  while (line != NULL)
    {
      FSTableLine *this_line;

      this_line = line;
      line = line->next;

      fs_table_line_free (this_line);
    }
  table->lines = NULL;

  table->parse_buffer[0] = '\0';
  free (table->parse_buffer);

  table->parse_buffer_length = 0;
  table->parse_buffer_capacity = 0;

  free (table);
}

static void
fs_table_line_update_pointer (FSTableLine *line, FSTableField *field)
{
  switch (field->type)
    {
    case FS_TABLE_FIELD_TYPE_WHITE_SPACE:
      break;
    case FS_TABLE_FIELD_TYPE_BLOCK_DEVICE:
      line->block_device = field->value;
      break;
    case FS_TABLE_FIELD_TYPE_MOUNT_POINT:
      line->mount_point = field->value;
      break;
    case FS_TABLE_FIELD_TYPE_FILE_SYSTEM_TYPE:
      line->fs_type = field->value;
      break;
    case FS_TABLE_FIELD_TYPE_MOUNT_OPTIONS:
      line->mount_options = field->value;
      break;
    case FS_TABLE_FIELD_TYPE_DUMP_FREQUENCY:
      line->dump_frequency = field->value;
      break;
    case FS_TABLE_FIELD_TYPE_PASS_NUMBER:
      line->pass_number = field->value;
      break;
    default:
      assert (FALSE);
      break;
    }
}

static boolean
fs_table_parse_line (FSTable *table, const char *line, size_t length)
{
  FSTableLine *table_line;
  FSTableField *field;
  char *field_value, *p;
  FSTableFieldType current_field;
  size_t i;

  table_line = fs_table_line_new ();

  current_field = FS_TABLE_FIELD_TYPE_BLOCK_DEVICE;
  p = (char *) line;
  i = 0;
  while (*p != '\0' && current_field <= FS_TABLE_FIELD_TYPE_WHITE_SPACE)
    {
      /* First grab the whitespace before the current field 
       */
      while (isspace (line[i]) && i < length)
        i++;

      /* If it's not before the first field or after the last field
       * then we don't want to accept the line ending with whitespace
       */
      if (current_field != FS_TABLE_FIELD_TYPE_WHITE_SPACE
          && (i > length || (line[i] == '\0' && current_field != 0)))
        {
          fstab_update_debug (_("%d: Line ended prematurely\n"), pid);
          return FALSE;
        }

      if (line + i != p)
        {
          field_value = strndup (p, line + i - p);
          field = fs_table_field_new (FS_TABLE_FIELD_TYPE_WHITE_SPACE,
                                      field_value);
          free (field_value);

          fs_table_line_add_field (table_line, field);
        }

      /* Now the actual field ...
       */
      p = (char *) line + i;

      /* If it's a comment, blank line, or end of line, 
       * grab the rest of the line, otherwise just go until whitespace
       */

      if (*p == '#' || 
          current_field == FS_TABLE_FIELD_TYPE_WHITE_SPACE)
        {
          field_value = strndup (p, length - i);
          field = fs_table_field_new (FS_TABLE_FIELD_TYPE_WHITE_SPACE,
                                      field_value);
          fs_table_line_add_field (table_line, field);
          break;
        }

      while (line[i] != '\0' && !isspace (line[i]) && i < length)
        i++;

      if (i > length || (line[i] == '\0' && i < length))
        {

          if (current_field == 0)
            break;
          else
            {
              fstab_update_debug (_("%d: Line ended prematurely\n"), pid);
              return FALSE;
            }
        }

      assert (line + i != p);

      field_value = strndup (p, line + i - p);
      field = fs_table_field_new (current_field, field_value);
      current_field++;
      free (field_value);

      fs_table_line_add_field (table_line, field);

      p = (char *) line + i;
    }

  fs_table_add_line (table, table_line);

  return TRUE;
}

static boolean
fs_table_flush_parse_buffer (FSTable *table)
{
  boolean retval;

  if (table->parse_buffer_length > 0) 
    {
      retval = fs_table_parse_line (table, table->parse_buffer,
                                    table->parse_buffer_length);

      table->parse_buffer[0] = '\0';
      table->parse_buffer_length = 0;

      return retval;
    }

  return TRUE;
}

static boolean 
fs_table_parse_data (FSTable *table, const char *data, size_t length)
{
  boolean retval;
  size_t i;

  for (i = 0; i < length; i++)
    {
      if (data[i] == '\n')
        {
	  /* When a newline is encountered flush the parse buffer so that the
	   * line can be parsed.  Note that completely blank lines won't show
	   * up in the parse buffer, so they get parsed directly.
	   */
	  if (table->parse_buffer_length > 0)
	    retval = fs_table_flush_parse_buffer (table);
	  else
	    retval = fs_table_parse_line (table, "", 1);

          if (retval == FALSE)
            return FALSE;
        }
      else
        {
          if (table->parse_buffer_length + 1 >= 
              table->parse_buffer_capacity - 1)
            {
              table->parse_buffer_capacity *= 2;

              table->parse_buffer = realloc (table->parse_buffer, 
                                             table->parse_buffer_capacity);

            }

          table->parse_buffer[table->parse_buffer_length++] = data[i];
          table->parse_buffer[table->parse_buffer_length] = '\0';
        }
    }

  return TRUE;
}

static inline int
get_random_int_in_range (int low, int high)
{
  return (random () % (high - low)) + low;
}

static int
open_temp_fstab_file (const char *dir, char **filename)
{
  char candidate_filename[TEMP_FSTAB_MAX_LENGTH] = { 0 };
  int fd;

  enum Choice 
    { 
      CHOICE_UPPER_CASE = 0, 
      CHOICE_LOWER_CASE, 
      CHOICE_DIGIT, 
      NUM_CHOICES
    } choice;

  strncpy (candidate_filename, TEMP_FSTAB_PREFIX,
           TEMP_FSTAB_MAX_LENGTH);

  /* Generate a unique candidate filename and open it for writing
   */
  srandom ((unsigned int) time (NULL));
  fd = -1;
  while (fd < 0)
    {
      char *full_path, buf[2] = { 0 };

      if (strlen (candidate_filename) > TEMP_FSTAB_MAX_LENGTH) 
        {
          strncpy (candidate_filename, TEMP_FSTAB_PREFIX,
                   TEMP_FSTAB_MAX_LENGTH);
          candidate_filename[strlen (TEMP_FSTAB_PREFIX)] = '\0';
        }

      choice = (enum Choice) get_random_int_in_range (0, NUM_CHOICES);

      switch (choice)
        {
        case CHOICE_UPPER_CASE:
          buf[0] = (char) get_random_int_in_range ('A' , 'Z' + 1);
          strcat (candidate_filename, buf);
          break;
        case CHOICE_LOWER_CASE:
          buf[0] = (char) get_random_int_in_range ('a' , 'z' + 1);
          strcat (candidate_filename, buf);
          break;
        case CHOICE_DIGIT:
          buf[0] = (char) get_random_int_in_range ('0' , '9' + 1);
          strcat (candidate_filename, buf);
          break;
        default:
          abort ();
          break;
        }

      full_path = (char *) malloc (strlen (dir) + 1 /* / */ + 
                                   strlen (candidate_filename) + 
                                   1 /* \0 */ );
      strcpy (full_path, dir);
      if (full_path[strlen (full_path) - 1] != '/')
        strcat (full_path, "/");
      strcat (full_path, candidate_filename);

      fstab_update_debug (_("%d: using temporary file '%s'\n"), pid, full_path);

      fd = open (full_path, O_CREAT | O_RDWR | O_EXCL, 0644); 

      if (fd < 0) 
        {
          if (errno != EEXIST)
            {
              fstab_update_debug (_("%d: Could not open temporary file for "
                                    "writing in directory '%s': %s\n"),
                                  pid, dir, strerror (errno));
              break;
            }
        }
      else if (fd >= 0 && filename != NULL)
        {
          *filename = strdup (full_path);
          break;
        }

      free (full_path);
    }

  return fd;
}


static char *
fixup_mount_point (const char *device_type)
{
  char *p, *q, *new_device_type;

  new_device_type = malloc (strlen (device_type) + 1);
  q = new_device_type;
  for (p = (char *) device_type; *p != '\0'; p++)
    {
      if (isspace (*p))
        *q = '_';
      else if (*p == '/')
        *q = '-';
      else
        {
          *q = *p;

          if (!isalpha(*q) && !isdigit(*q) && *q!='_' && *q!='-')
            *q = '_';
        }

      q++;
    }
  *q = '\0';

  return new_device_type;
}

/* FIXME: This function should be more fleshed out then it is
 *
 */
static boolean
create_mount_point_for_volume (const char *full_mount_path)
{

  /* FIXME: Should only mkdir if we need to and should do so
   * recursively for each component of the mount root.
   */
  mkdir (fsy_mount_root, 0775);

  return (mkdir (full_mount_path, 0775) != -1) || errno == EEXIST;
}


/** See if the fstab already got this device listed. The parameter label
 *  can be NULL.
 *
 */
static boolean
fs_table_has_device (FSTable *table, const char *block_device, const char *label, const char *udi)
{
  FSTableLine *line;
  struct stat statbuf;

  line = table->lines;
  while (line != NULL)
    {
      FSTableField *field;

      field = line->fields;
      while (field != NULL)
        {

          if (field->type == FS_TABLE_FIELD_TYPE_BLOCK_DEVICE)
            {

	      /* Easy, the device file is a match */
              if (strcmp (field->value, block_device) == 0)
                return TRUE;

	      /* Check if it's a symlink */
	      if (lstat (field->value, &statbuf) == 0) {

		if (S_ISLNK(statbuf.st_mode)) {
		  char buf[256];

		  memset (buf, '\0', sizeof (buf));

		  if (readlink (field->value, buf, sizeof (buf)) > 0) {

		    /* check if link is fully qualified  */
		    if (buf[0] != '/') {
		      char buf1[256];
		      char *p;

		      strncpy (buf1, buf, sizeof (buf1));
		      strncpy (buf, field->value, sizeof(buf));
		      p = strrchr (buf, '/');
		      strncpy (p+1, buf1, buf+sizeof(buf)-p-1);
		    }

		    if (strcmp (block_device, buf) == 0) {
			    DBusError error;
			    /* update block.device with new value */
			    fstab_update_debug (_("%d: Found %s pointing to %s in" _PATH_FSTAB), pid, field->value, buf);
			    dbus_error_init (&error);
			    libhal_device_set_property_string (hal_context, udi, "block.device", field->value, &error);
		      return TRUE;
		    }

		  }
		} 
	      }

	      /* Mount by label, more tricky, see below... */
	      if (strncmp (field->value, "LABEL=", 6) == 0 &&
		  strlen (field->value) > 6 && label != NULL &&
		  strcmp (field->value + 6, label) == 0) {
		FSTableField *i;
		char *mount_point;
      
		/* OK, so this new volume has a label that is matched
		 * in the fstab.. Check, via /etc/mtab, whether the
		 * device file for the entry in mtab matches the mount
		 * point 
		 *
		 * (If it's mounted at all, which we assume it
		 * is since no hotpluggable drives should be listed
		 * in /etc/fstab by LABEL. And note that if it's 
		 * not mounted then now we have TWO volumes with
		 * the same label and then everything is FUBAR anyway) 
		 */

		/* first, find the mountpoint from fstab */
		mount_point = NULL;
		for (i = line->fields; i != NULL; i = i->next) {
		  if (i->type == FS_TABLE_FIELD_TYPE_MOUNT_POINT) {
		    mount_point = i->value;
		    break;
		  }
		}

		if (mount_point != NULL) {
		  FILE *f;
		  struct mntent mnt;
		  struct mntent *mnte;
		  char buf[512];
		  char *device_file_from_mount_point;

		  /* good, now lookup in /etc/mtab */
		  device_file_from_mount_point = NULL;
		  if ((f = setmntent ("/etc/mtab", "r")) != NULL) {

		    while ((mnte = getmntent_r (f, &mnt, buf, sizeof(buf))) != NULL) {
		      if (strcmp (mnt.mnt_dir, mount_point) == 0) {
			device_file_from_mount_point = mnt.mnt_fsname;
			break;
		      }
		    }

		    endmntent (f);
		  }

		  /* now see if it's the same device_file as our volume */
		  if (device_file_from_mount_point != NULL &&
		      strcmp (device_file_from_mount_point, 
			      block_device) == 0) {
		    /* Yah it is.. So we're already listed in the fstab */
		    return TRUE;
		  }
		}
	      } /* entry is LABEL= */

            }

          field = field->next;
        }
      line = line->next;
    }

  return FALSE;
}

#define OPTIONS_SIZE 256

/* safely strcat() at most the remaining space in 'dst' */
#define strcat_len(dst, src) do { \
	dst[sizeof (dst) - 1] = '\0'; \
	strncat (dst, src, sizeof (dst) - strlen (dst) - 1); \
} while(0)

static boolean
fs_table_line_has_mount_option (FSTableLine *line, const char *option)
{
  char *p, *q;

  if (line->mount_options == NULL)
    return FALSE;

  p = line->mount_options;
  q = p;
  while (*q != '\0')
    {
      while (*q != ',' && *q != '\0')
        q++;

      if (strncmp (option, p, q - p) == 0)
        return TRUE;

      q++;
      p = q;
    }

  return FALSE;
}

/* Note this function returns the line of the fs table that was
 * removed for convenience (a bit strange, I know)
 */
static FSTableLine *
fs_table_remove_volume (FSTable *table, const char *block_device)
{
  FSTableLine *line, *previous_line; 

  previous_line = NULL;
  line = table->lines;
  while (line != NULL)
    {
      if (line->block_device != NULL
          && strcmp (line->block_device, block_device) == 0
          && fs_table_line_is_generated (line))
        {
          if (previous_line == NULL)
              table->lines = line->next;
          else 
            {
              if (line->next == NULL)
                table->tail = previous_line;
              previous_line->next = line->next;
            }

          return line;
        }

      previous_line = line;
      line = line->next;
    }

  return NULL;
}

static time_t
get_file_modification_time (const char *filename)
{
  struct stat buf = { 0 };

  if (stat (filename, &buf) < 0)
    {
      fstab_update_debug (_("%d: Could not stat '%s': %s\n"),
                          pid, filename, strerror (errno));
    }
  
  return buf.st_mtime;
}

#define FSTAB_SYNC_BOILER_PLATE_LINE "# This file is edited by fstab-sync - see 'man fstab-sync' for details"

/** Add boilerplate about us editing the fstab file if not
 *  already there
 */
static void 
append_boilerplate (FSTable *table)
{
	FSTableLine *line, *previous_line; 
	FSTableField *field;

	previous_line = NULL;
	line = table->lines;
	while (line != NULL)
	{
		for (field = line->fields; field != NULL; field = field->next) {
			if (field->type == FS_TABLE_FIELD_TYPE_WHITE_SPACE) {
				if (strcmp (field->value, FSTAB_SYNC_BOILER_PLATE_LINE) == 0)
					return;
			}
		}
		line = line->next;
	}

	line = fs_table_line_new ();
	if (line == NULL)
		return;
	field = fs_table_field_new (FS_TABLE_FIELD_TYPE_WHITE_SPACE, FSTAB_SYNC_BOILER_PLATE_LINE);
	if (field == NULL)
		return;
	fs_table_line_add_field (line, field);

	/* add to start of file */
	if (table->lines == NULL)
	{
		table->lines = line;
		table->tail = line;
	}
	else
	{
		line->next = table->lines;
		table->lines = line;
	}
	
	
}

static char *
compute_mount_point (FSTable *table, const char *desired)
{
  FSTableLine *line;
  char desired_name[256];
  int dev_number;
  struct stat statbuf;  

  dev_number = 0;

tryagain:
  if (dev_number >= 16384) {
    /* to prevent looping; should never happen (unless you got more than 16k disks) */
    return NULL;
  }

  if (dev_number == 0)
    snprintf (desired_name, sizeof (desired_name), "%s/%s", fsy_mount_root, desired);
  else
    snprintf (desired_name, sizeof (desired_name), "%s/%s%d", fsy_mount_root, desired, dev_number);

  /* see if it's in fstab */
  for (line = table->lines; line != NULL; line = line->next) {
    if (line->mount_point != NULL && strcmp (line->mount_point, desired_name) == 0) {
      dev_number++;
      goto tryagain;
    }
  }
  
  /* see if the mount point physically exists */
  if (stat (desired_name, &statbuf) == 0 || errno != ENOENT) {
    dev_number++;
    goto tryagain;
  }

  return strdup (desired_name);
}


/** Add a hal device object to the fstab if applicable.
 *
 *  Returns NULL if we don't want to add this hal device. 
 *  Otherwise the fully qualified mount point is returned and
 *  it must be freed by the caller.
 */
static char* add_hal_device (FSTable *table, const char *udi)
{
	char *rc;
	HalDrive *drive;
	HalVolume *volume;
	FSTableLine *line;
	char final_options[256];
	
	drive = NULL;
	volume = NULL;
	rc = NULL;
	
	fstab_update_debug (_("%d: entering add_hal_device, udi='%s'\n"), pid, udi);
	
	drive  = hal_drive_from_udi (hal_context, udi);
	/*fstab_update_debug (_("%d: drive=%x, volume=%x\n"), pid, drive, volume);*/
	if (drive == NULL) {
		char *udi_storage_device;
		DBusError error;
		/* try block.storage_device */
		dbus_error_init (&error);
		udi_storage_device = libhal_device_get_property_string (hal_context, udi, "block.storage_device", &error);
		if (udi_storage_device == NULL)
			goto out;
		drive = hal_drive_from_udi (hal_context, udi_storage_device);
		libhal_free_string (udi_storage_device);
		if (drive == NULL)
			goto out;
	}

	volume = hal_volume_from_udi (hal_context, udi);
		
	/* see if we are a drive */
	if (volume == NULL) {
		const char *device_file;
		char *mount_point;
		char *normalized_desired_mount_point;
		const char *desired_mount_point;
		const char *fstype;
		const char *options;
		
		if (!hal_drive_policy_is_mountable (drive, NULL))
			goto out;
		
		device_file = hal_drive_get_device_file (drive);
		if (device_file == NULL)
			goto out;

		if (fs_table_has_device (table, device_file, NULL, udi)) /* no label */ {
			fstab_update_debug (_("%d: Could not add entry to fstab file: "
					      "block device %s already listed\n"), pid, device_file);
			goto out;
		}
		
		desired_mount_point = hal_drive_policy_get_desired_mount_point (drive, NULL);
		if (desired_mount_point == NULL)
			goto out;

		normalized_desired_mount_point = fixup_mount_point (desired_mount_point);
		if (normalized_desired_mount_point == NULL)
			goto out;
		
		fstype = hal_drive_policy_get_mount_fs (drive, NULL);
		if (fstype == NULL)
			goto out;
		options = hal_drive_policy_get_mount_options (drive, NULL);
		if (options == NULL)
			goto out;
		
		mount_point = compute_mount_point (table, normalized_desired_mount_point);
		if (mount_point == NULL)
			goto out;
				
		if (fsy_use_managed) {
			snprintf (final_options, sizeof (final_options), "%s,%s", options, fsy_managed_primary);
		} else {
			snprintf (final_options, sizeof (final_options), "%s", options);
		}

		fstab_update_debug (_("%d: drive: desired_mount_point='%s', fstype='%s', options='%s', "
				      "mount_point='%s', normalized_desired_mount_point='%s'\n"), 
				    pid, desired_mount_point, fstype, options, mount_point, 
				    normalized_desired_mount_point);

		line = fs_table_line_new_from_field_values (device_file,
							    mount_point,
							    fstype,
							    final_options, 0, 0);
		if (line == NULL)
			goto out;
		fs_table_add_line (table, line);

		rc = mount_point;
	} else {
		/* otherwise we're a volume */
		const char *device_file;
		char *mount_point;
		char *normalized_desired_mount_point;
		const char *desired_mount_point;
		const char *label;
		const char *fstype;
		const char *options;
		
		/* means we are a volume */
		if (!hal_volume_policy_is_mountable (drive, volume, NULL))
			goto out;
		
		device_file = hal_volume_get_device_file (volume);
		if (device_file == NULL)
			goto out;
		
		label = hal_volume_get_label (volume);
		if (device_file == NULL)
			goto out;
		
		if (fs_table_has_device (table, device_file, label, udi)) /* no label */ {
			fstab_update_debug (_("%d: Could not add entry to fstab file: "
					      "block device %s already listed\n"), pid, device_file);
			goto out;
		}
		
		desired_mount_point = hal_volume_policy_get_desired_mount_point (drive, volume, NULL);
		if (desired_mount_point == NULL)
			goto out;

		normalized_desired_mount_point = fixup_mount_point (desired_mount_point);
		if (normalized_desired_mount_point == NULL)
			goto out;
		
		fstype = hal_volume_policy_get_mount_fs (drive, volume, NULL);
		if (fstype == NULL)
			goto out;
		options = hal_volume_policy_get_mount_options (drive, volume, NULL);
		if (options == NULL)
			goto out;
		
		mount_point = compute_mount_point (table, normalized_desired_mount_point);
		if (mount_point == NULL)
			goto out;
		
		fstab_update_debug (_("%d: volume: desired_mount_point='%s', fstype='%s', options='%s', mount_point='%s'\n"), 
				    pid, desired_mount_point, fstype, options, mount_point);
		
		if (fsy_use_managed) {
			snprintf (final_options, sizeof (final_options), "%s,%s", options, fsy_managed_primary);
		} else {
			snprintf (final_options, sizeof (final_options), "%s", options);
		}
		
		fstab_update_debug (_("%d: drive: desired_mount_point='%s', fstype='%s', options='%s', "
				      "mount_point='%s', normalized_desired_mount_point='%s'\n"), 
				    pid, desired_mount_point, fstype, options, mount_point, 
				    normalized_desired_mount_point);
		
		line = fs_table_line_new_from_field_values (device_file,
							    mount_point,
							    fstype,
							    final_options, 0, 0);
		if (line == NULL)
			goto out;
		fs_table_add_line (table, line);
		
		rc = mount_point;
	}
	
	
out:
	hal_volume_free (volume);
	hal_drive_free (drive);
	return rc;
}


static boolean
add_udi (const char *udi)
{
	FSTable *fs_table;
	char *temp_filename = NULL;
	time_t fstab_modification_time;
	int fd = -1;
	char *dir = NULL;
	char *last_slash;
	char *mount_point;
	char *device_file;
	DBusError error;
	
	dir = strdup (_PATH_FSTAB); 	 
	last_slash = strrchr (dir, '/'); 	 
	if (last_slash) 
		*last_slash = '\0';
	fs_table = fs_table_new (_PATH_FSTAB);
	if (fs_table == NULL)
		goto error;
	
	fd = open_temp_fstab_file (dir, &temp_filename);
	
	if (fd < 0)
		goto error;
	
	mount_point = add_hal_device (fs_table, udi);
	if (mount_point == NULL)
		goto error;

	append_boilerplate (fs_table);
	
	fstab_modification_time = get_file_modification_time (_PATH_FSTAB);
	
	if (fstab_modification_time == 0)
		goto error;
	
	if (!fs_table_write (fs_table, fd))
		goto error;
	
	/* Someone changed the fs table under us, better start over.
	 */
	if (get_file_modification_time (_PATH_FSTAB) != fstab_modification_time)
	{
		close (fd);
		unlink (temp_filename);
		return add_udi (udi);
	}
	
	if (rename (temp_filename, _PATH_FSTAB) < 0)
	{
		fstab_update_debug (_("%d: Failed to rename '%s' to '%s': %s\n"),
				    pid, temp_filename, _PATH_FSTAB, strerror (errno));
		goto error;
	}
	
	if (!create_mount_point_for_volume (mount_point))
		goto error;

	dbus_error_init (&error);
	device_file = libhal_device_get_property_string (hal_context, udi, "block.device", &error);
	
	fstab_update_debug (_("%d: added mount point '%s' for device '%s'\n"),
			    pid, mount_point, device_file);
	syslog (LOG_INFO, _("added mount point %s for %s"), 
		mount_point, device_file);
	
	libhal_free_string (device_file);
	
	close (fd);
	
	return TRUE;
	
error:
	if (fd >= 0)
		close (fd);
	if (dir != NULL)
		free (dir);
	if (temp_filename != NULL)
		unlink (temp_filename);
	return FALSE;
}

static boolean
remove_udi (const char *udi)
{
  char *block_device;
  FSTable *fs_table = NULL;
  FSTableLine *line = NULL;
  char *temp_filename = NULL;
  time_t fstab_modification_time;
  int fd;
  boolean is_volume;
  char *dir = NULL;
  char *last_slash;
  DBusError error;

  dbus_error_init (&error);

  is_volume = libhal_device_query_capability (hal_context, udi, "volume", &error);

  /* don't remove the fstab entry if we were spawned of a device with
   * storage.no_partitions_hint set to TRUE. Per the spec this is
   * exactly when block.no_partitions is TRUE on the volume. E.g.
   * floppies and optical discs
   */
  if (is_volume && libhal_device_get_property_bool (hal_context, udi, "block.no_partitions", &error))
    return FALSE;

  block_device = libhal_device_get_property_string (hal_context, udi, "block.device", &error);

  dir = strdup (_PATH_FSTAB); 	 
  last_slash = strrchr (dir, '/'); 	 
  if (last_slash) 
    *last_slash = '\0';

  fs_table = fs_table_new (_PATH_FSTAB);

  fd = open_temp_fstab_file (dir, &temp_filename);

  if (fd < 0)
    goto error;

  fstab_modification_time = get_file_modification_time (_PATH_FSTAB);

  if (fstab_modification_time == 0)
    goto error;

  line = fs_table_remove_volume (fs_table, block_device);

  if (line == NULL)
    {
      fstab_update_debug (_("%d: Could not remove device '%s' with UDI '%s' from "
                            "fs table: not found\n"),
                          pid, block_device, udi);
      goto error;
    }

  assert (line->mount_point != NULL);

  if (rmdir (line->mount_point) < 0)
    {
      fstab_update_debug (_("%d: Failed to remove mount point '%s': %s\n"),
                          pid, line->mount_point, strerror (errno));
      goto error;
    }


  append_boilerplate (fs_table);

  if (!fs_table_write (fs_table, fd))
    goto error;

  /* Someone changed the fs table under us, better start over.
   */
  if (get_file_modification_time (_PATH_FSTAB) != fstab_modification_time)
    {
      close (fd);
      unlink (temp_filename);
      free (block_device);
      return remove_udi (udi);
    }

  if (rename (temp_filename, _PATH_FSTAB) < 0)
    {
      fstab_update_debug (_("%d: Failed to rename '%s' to '%s': %s\n"),
                          pid, temp_filename, _PATH_FSTAB, strerror (errno));
      goto error;
    }

  fstab_update_debug (_("%d: removed mount point for device '%s'\n"),
		      pid, block_device);
  syslog (LOG_INFO, _("removed mount point %s for %s"), line->mount_point, block_device);

  close (fd);
  free (block_device);
  fs_table_line_free (line);

  return TRUE;

error:
  if (fd >= 0)
    close (fd);

  free (block_device);

  if (temp_filename != NULL)
    unlink (temp_filename);

  if (dir != NULL)
    free (dir);

  fs_table_line_free (line);

  return FALSE;
}

static boolean fs_table_line_is_mounted (FSTableLine *line)
{
  FILE *f = NULL;
  boolean is_mounted = FALSE;
  struct mntent *m;

  if (line->block_device == NULL || line->mount_point == NULL)
	  goto out;

  f = fopen (_PATH_MOUNTED, "r");
  if (f == NULL)
    goto out;

  while ((m = getmntent (f)) != NULL) {

    if (strcmp (m->mnt_fsname, line->block_device) == 0 && 
	strcmp (m->mnt_dir, line->mount_point) == 0) {
      is_mounted = TRUE;
      goto out;
    }
  }

out:
  if (f != NULL)
    fclose (f);

  return is_mounted;
}

static void
fs_table_remove_generated_entries (FSTable *table)
{
  FSTableLine *line, *previous_line; 

  previous_line = NULL;
  line = table->lines;
  while (line != NULL)
    {
	    fstab_update_debug (_("%d: Seeing if line for dev='%s',mnt='%s' should be removed\n"), pid, line->block_device, line->mount_point);
	    fstab_update_debug (_("%d: is_generated=%d, is_mounted=%d\n"), pid, 
				fs_table_line_is_generated (line), fs_table_line_is_mounted (line));
      /* don't remove generated line if device is mounted there */
      if (fs_table_line_is_generated (line) && !fs_table_line_is_mounted (line))
        {

	  if (rmdir (line->mount_point) < 0)
	    {
	      fstab_update_debug (_("%d: Failed to remove mount point '%s': %s\n"),
				  pid, line->mount_point, strerror (errno));
	    }


          if (previous_line == NULL)
	    {
              table->lines = line->next;
	    }
          else 
            {
              if (line->next == NULL)
                table->tail = previous_line;

              previous_line->next = line->next;
            }
        }
      else
	{
	  previous_line = line;
	}

      line = line->next;
    }

}

static boolean
clean (void)
{
  FSTable *fs_table;
  char *temp_filename = NULL;
  time_t fstab_modification_time;
  int fd;
  char *dir = NULL;
  char *last_slash;

  dir = strdup (_PATH_FSTAB); 	 
  last_slash = strrchr (dir, '/'); 	 
  if (last_slash) 
    *last_slash = '\0';

  fs_table = fs_table_new (_PATH_FSTAB);

  fd = open_temp_fstab_file (dir, &temp_filename);

  if (fd < 0)
    goto error;

  fstab_modification_time = get_file_modification_time (_PATH_FSTAB);

  if (fstab_modification_time == 0)
    goto error;

  fs_table_remove_generated_entries (fs_table);

  append_boilerplate (fs_table);

  if (!fs_table_write (fs_table, fd))
    goto error;

  /* Someone changed the fs table under us, better start over.
   */
  if (get_file_modification_time (_PATH_FSTAB) != fstab_modification_time)
    {
      close (fd);
      unlink (temp_filename);
      return clean ();
    }

  if (rename (temp_filename, _PATH_FSTAB) < 0)
    {
      fstab_update_debug (_("%d: Failed to rename '%s' to '%s': %s\n"),
                          pid, temp_filename, _PATH_FSTAB, strerror (errno));
      goto error;
    }

  close (fd);

  syslog (LOG_INFO, _("removed all generated mount points"));

  return TRUE;

error:
  if (fd >= 0)
    close (fd);
  if (dir != NULL)
    free (dir);
  if (temp_filename != NULL)
    unlink (temp_filename);
  return FALSE;
}

int
main (int argc, const char *argv[])
{
  int i, retval = 0;
  poptContext popt_context;
  boolean should_clean = FALSE;
  char *udi_to_add = NULL, *udi_to_remove = NULL, *hal_device_udi;
  const char **left_over_args = NULL;
  int lockfd = -1;
  DBusError error;
  DBusConnection *conn;

  pid = getpid ();

  openlog (PROGRAM_NAME, LOG_PID, LOG_USER);
  
  struct poptOption options[] = {
      {"add",     'a', POPT_ARG_STRING, &udi_to_add,    0, N_("add an entry to fstab"), N_("UDI")},
      {"remove",  'r', POPT_ARG_STRING, &udi_to_remove, 0, N_("remove an entry from fstab"), N_("UDI")},
      {"clean",   'c', POPT_ARG_NONE,   &should_clean,  0, N_("Remove all generated entries from fstab"), NULL},
      {"verbose", 'v', POPT_ARG_NONE,   &verbose,       0, N_("Report detailed information about operation progress"), NULL},

      POPT_AUTOHELP

      {NULL, '\0', 0, NULL, 0, NULL, NULL}
  };

  popt_context = poptGetContext (PROGRAM_NAME, argc, argv, options, 0);
	  
  while ((i = poptGetNextOpt (popt_context)) != -1)
    {
      if (i < -1)
        {
	  poptPrintHelp (popt_context, stderr, 0);
          return 1;
        }
    }

  if (getenv ("HALD_VERBOSE") != NULL)
      verbose = TRUE;

  /* accept "add" and "remove" / HAL environment variables 
   * in addition to "--add" and "--remove" above
   */
  left_over_args = poptGetArgs (popt_context);

  hal_device_udi = getenv ("UDI");
  if (hal_device_udi != NULL) {
      char *caps;
      
      /* when invoked for the /org/freedesktop/Hal/devices/computer UDI we clean the fstab */
      if (getenv ("HALD_STARTUP") != NULL && strcmp (hal_device_udi, "/org/freedesktop/Hal/devices/computer") == 0) {
	should_clean = TRUE;
      } else {
      
	/* when we are invoked by hald, make some early tests using the
	 * exported environment so we don't have to connect to hald */
	
	caps = getenv ("HAL_PROP_INFO_CAPABILITIES");
	
	/* if there are no info.capabilities just bail out */
	if (caps == NULL) {
	  retval = 0;
	  goto out;
	}
	
	/* we only handle hal device objects of capability 'volume' or 'storage' */
	if (caps != NULL) {
	  
	  if (strstr (caps, "volume") == NULL &&
	      strstr (caps, "storage") == NULL) {
	    retval = 0;
	    goto out;
	  }
	}

	/* we don't want to remove entries just because hald is shutting down */
	if (getenv ("HALD_SHUTDOWN") != NULL)
	  goto out;
      }


      fstab_update_debug (_("%d: ###################################\n"), pid);
      fstab_update_debug (_("%d: %s entering; %s udi=%s\n"), 
			  pid, PROGRAM_NAME, argv[1], hal_device_udi);
      
      lockfd = open (_PATH_FSTAB, O_RDONLY);
      if (lockfd < 0) {
	fstab_update_debug (_("%d: couldn't open %s O_RDONLY; bailing out\n"), 
			    pid, _PATH_FSTAB);
	retval = 1;
	goto out;
      }
      fstab_update_debug (_("%d: Acquiring advisory lock on " 
			    _PATH_FSTAB "\n"), pid);
      if (flock (lockfd, LOCK_EX) != 0) {
	fstab_update_debug (_("%d: Error acquiring lock '%s'; bailing out\n"),
			    pid, strerror(errno));
	retval = 1;
	goto out;
      }
      fstab_update_debug (_("%d: Lock acquired\n"), pid);
    }
  

  if (!should_clean && left_over_args)
	  for (i = 0; left_over_args[i] != NULL; i++)
	  {
		  if (strcmp (left_over_args[i], "add") == 0 && udi_to_add == NULL)
		  {
			  udi_to_add = strdup (hal_device_udi);
			  break;
		  }
		  
		  if (strcmp (left_over_args[i], "remove") == 0)
		  {
			  udi_to_remove = strdup (hal_device_udi);
			  break;
		  }
	  }

  dbus_error_init (&error);	
  conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
  if (conn == NULL) {
	  fprintf (stderr, "error: dbus_bus_get: %s: %s\n", error.name, error.message);
	  goto out;
  }		
  if ((hal_context = libhal_ctx_new ()) == NULL) {
	  fprintf (stderr, "error: libhal_ctx_new\n");
	  goto out;
  }
  if (!libhal_ctx_set_dbus_connection (hal_context, conn)) {
	  fprintf (stderr, "error: libhal_ctx_set_dbus_connection: %s: %s\n", error.name, error.message);
	  goto out;
  }
  if (!libhal_ctx_init (hal_context, &error)) {
	  fprintf (stderr, "error: libhal_ctx_init: %s: %s\n", error.name, error.message);
	  goto out;
  }

  fsy_mount_root = hal_drive_policy_default_get_mount_root (hal_context);
  if (fsy_mount_root == NULL)
	  goto out;
  fstab_update_debug (_("%d: mount_root='%s'\n"), pid, fsy_mount_root);
  fsy_use_managed = hal_drive_policy_default_use_managed_keyword (hal_context);
  fstab_update_debug (_("%d: use_managed=%d\n"), pid, fsy_use_managed);
  if (fsy_use_managed) {
	  if ((fsy_managed_primary   = hal_drive_policy_default_get_managed_keyword_primary (hal_context)) == NULL)
		  goto out;
	  if ((fsy_managed_secondary = hal_drive_policy_default_get_managed_keyword_secondary (hal_context)) == NULL)
		  goto out;
	  fstab_update_debug (_("%d: managed primary='%s'\n"), pid, fsy_managed_primary);
	  fstab_update_debug (_("%d: managed secondary='%s'\n"), pid, fsy_managed_secondary);
  }
  
  if (!should_clean && (udi_to_add || udi_to_remove))
    {
      if (udi_to_add)
        retval = !add_udi (udi_to_add);

      if (udi_to_remove)
        retval |= !remove_udi (udi_to_remove);

      libhal_ctx_shutdown (hal_context, &error);
      libhal_ctx_free (hal_context);
    }
  else if (should_clean)
    retval = clean ();
  else
    {
	poptPrintHelp (popt_context, stderr, 0);
	return 1;
    }

  if (hal_device_udi != NULL) {

    fstab_update_debug (_("%d: Releasing advisory lock on %s\n"), 
			pid, _PATH_FSTAB);
    if (flock (lockfd, LOCK_EX) != 0) {
      fstab_update_debug (_("%d: Error releasing lock '%s'\n"), pid, 
	      strerror(errno));
      retval = 1;
    } else {
      fstab_update_debug (_("%d: Lock released\n"), pid);
    }
    close (lockfd);

    fstab_update_debug (_("%d: %s exiting; %s udi=%s\n"), 
			pid, PROGRAM_NAME, argv[1], hal_device_udi);

    fstab_update_debug (_("%d: ###################################\n"), pid);
    fstab_update_debug (_("\n"));
  }

  poptFreeContext (popt_context);

out:
  return retval;
}
