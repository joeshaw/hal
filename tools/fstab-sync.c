/* Copyright 2004 Red Hat, Inc.
 *
 * This software may be freely redistributed under the terms of the GNU
 * Lesser General Public license.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* This program serves two major purposes:
 *    1) Update the fs table in response to HAL events
 *    2) Possibly mount the devices that were added in response to 1)
 *       (not useful if a volume manager is installed)
 *
 * Additionally, this program offers a third option of removing
 * any trace of its previous actions from the fs table.
 *
 * Because it is possible that this program could be invoked multiple
 * times (at bootup or when a user plugs in multiple pieces of
 * hardware), it is very important that all the different instances of
 * this program do not step on each others toes.  Also, it is important
 * that this program does not corrupt the fs table in the event of
 * error conditions like out-of-disk-space and power outages.  
 *
 * For these reasons, all operations are done on temporary copies of
 * /etc/fstab. After any particular instance is done it copies its temporary
 * file over to /etc/fstab.  If another process detects that fstab has
 * changed, then it simply starts over, recopying /etc/fstab and trying again.
 */

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
#include <time.h>
#include <unistd.h>

#include <popt.h>

#define DBUS_API_SUBJECT_TO_CHANGE
#include "hal/libhal.h"

#define _(a) (a)
#define N_(a) a

#define PROGRAM_NAME "update-fstab"
#define TEMP_FSTAB_PREFIX ".fstab.hal."
#define TEMP_FSTAB_MAX_LENGTH 64
#define MOUNT_ROOT "/media/"

#define LOCK_TIMEOUT 60          /* seconds */
#define LOCK_TIMEOUT_WAIT 500000 /* microseconds */

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE !TRUE
#endif

#define fstab_update_debug(...) if (verbose) fprintf (stderr, __VA_ARGS__)

typedef int boolean;

typedef enum
{
  DEVICE_TYPE_UNKNOWN = 0,
  DEVICE_TYPE_DISK,
  DEVICE_TYPE_CDROM,
} DeviceType;

typedef struct
{
  char *udi;
  char *block_device;
  char *type;
  char *fs_type;
  char *mount_point;

  DeviceType device_type;
} Volume;

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
static boolean verbose = FALSE;

static void fs_table_line_add_field (FSTableLine *line, FSTableField *field);
static boolean fs_table_line_is_generated (FSTableLine *line);
static void fs_table_line_update_pointer (FSTableLine *line, FSTableField *field);

static FSTable *fs_table_new (const char *filename);
static void fs_table_free (FSTable *table);
static boolean fs_table_parse_data (FSTable    *table, 
                                    const char *data, 
                                    size_t      length);


static inline int get_random_int_in_range (int low, int high);
static int open_and_lock_file (const char *filename);
static int open_temp_fstab_file (const char *dir, char **filename);

static char *get_hal_string_property (const char *udi, const char *property);
static boolean mount_device (const char *mount_point);
static boolean udi_is_volume (const char *udi);
static Volume *volume_new (const char *udi);
static void volume_determine_device_type (Volume *volume);
static void volume_free (Volume *volume);
static boolean create_mount_point_for_volume (Volume *volume);
static boolean fs_table_add_volume (FSTable *table, Volume *volume);
static FSTableLine *fs_table_remove_volume (FSTable *table, Volume *volume);

static boolean add_udi (const char *udi, boolean should_mount_device);
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

#ifdef USE_NOOP_MOUNT_OPTION
  if (!fs_table_line_has_mount_option (line, "kudzu"))
    return FALSE;
#endif

  if (strncmp (line->mount_point, MOUNT_ROOT, sizeof (MOUNT_ROOT) -1) != 0)
    return FALSE;

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

  input_fd = open_and_lock_file (filename);

  if (input_fd < 0)
    goto error;

  while ((bytes_read = read (input_fd, read_buf, 1024)) != 0)
    {
      if (bytes_read < 0)
        {
          if (errno == EINTR)
            continue;

          fstab_update_debug (_("Could not read from '%s': %s\n"),
                              filename, strerror (errno));
          goto error;
        }

      if (!fs_table_parse_data (table, read_buf, (size_t) bytes_read))
        {
          fstab_update_debug (_("Could not parse data from '%s'\n"), filename);
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

          fstab_update_debug (_("Could not write to temporary file: %s\n"),
                              strerror (errno));
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
          fstab_update_debug (_("Line ended prematurely\n"));
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
              fstab_update_debug (_("Line ended prematurely\n"));
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

      fd = open (full_path, O_CREAT | O_RDWR | O_EXCL, 0644); 

      if (fd < 0) 
        {
          if (errno != EEXIST)
            {
              fstab_update_debug (_("Could not open temporary file for "
                                    "writing in directory '%s': %s\n"),
                                  dir, strerror (errno));
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

static boolean
mount_device (const char *mount_point)
{
  char *argv[] = { "/bin/mount", NULL, NULL };
  pid_t pid;
  int status;

  argv[1] = (char *) mount_point;

  if (!(pid = fork ()))
    {
      execv (argv[0], argv);
      _exit (1);
    }

  waitpid (pid, &status, 0);

  if (!WIFEXITED (status) || WEXITSTATUS (status))
    {
      fstab_update_debug (_("/bin/mount failed\n"));
      return FALSE;
    }

  return TRUE;
}

static char *
get_hal_string_property (const char *udi, const char *property)
{
  char *value;

  value = NULL;

  if (hal_device_property_exists (hal_context, udi, property))
    value = hal_device_get_property_string (hal_context, udi, property);

  return value;
}

static boolean
udi_is_volume (const char *udi)
{
  if (!hal_device_query_capability (hal_context, udi, "block"))
    return FALSE;

  if (!hal_device_query_capability (hal_context, udi, "volume"))
    return FALSE;

  if (hal_device_property_exists (hal_context, udi, "volume.disc.has_data") &&
      !hal_device_get_property_bool (hal_context, udi, "volume.disc.has_data"))
    return FALSE;

  return hal_device_property_exists (hal_context, udi, "block.is_volume") && 
         hal_device_get_property_bool (hal_context, udi, "block.is_volume");
}

static char *
device_type_normalize (const char *device_type)
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
          *q = tolower (*p);

          if (*q < 'a' || *q > 'z')
            *q = '.';
        }
      q++;
    }
  *q = '\0';

  return new_device_type;
}

static void
volume_determine_device_type (Volume *volume)
{
  char *storage_device_udi;

  volume->type = NULL;

  storage_device_udi = get_hal_string_property (volume->udi,
                                                "block.storage_device");

  if (storage_device_udi)
    volume->type = get_hal_string_property (storage_device_udi, 
                                            "storage.drive_type");

  if (volume->type == NULL)
    volume->type = strdup ("device");

  /* HAL's storage.drive_type key isn't very specific.  Let's do some
   * heuristics to come up with something more specific if possible
   */
  if (strcmp (volume->type, "disk") == 0
      && hal_device_property_exists (hal_context, storage_device_udi, 
                                     "storage.removable")
      && hal_device_get_property_bool (hal_context, storage_device_udi,
                                       "storage.removable"))
    {
      char *physical_device_udi;

      physical_device_udi = get_hal_string_property (storage_device_udi, 
                                                     "storage.physical_device");

      if (physical_device_udi)
        {
          char *product;
          product = get_hal_string_property (physical_device_udi, 
                                             "info.product");
          free (volume->type);
          volume->type = device_type_normalize (product);
          free (product);
        }

      volume->device_type = DEVICE_TYPE_DISK;
    }
  else if (strcmp (volume->type, "cdrom") == 0)
    {
      char *disc_type;

      disc_type = get_hal_string_property (volume->udi, "volume.disc.type");

      if (disc_type)
        {
          free (volume->type);
          volume->type = disc_type;
        }
      volume->device_type = DEVICE_TYPE_CDROM;
    }

  if (storage_device_udi)
    free (storage_device_udi);
}

static Volume *
volume_new (const char *udi)
{
  Volume *volume;

  if (!udi_is_volume (udi))
    return NULL;

  volume = calloc (sizeof (Volume), 1);

  volume->udi = strdup (udi);

  volume->block_device = get_hal_string_property (udi, "block.device");

  if (volume->block_device == NULL)
    {
      volume_free (volume);
      return NULL;
    }

  volume->type = NULL;

  volume_determine_device_type (volume);

  volume->fs_type = get_hal_string_property (udi, "block.fs_type");

  if (volume->fs_type == NULL)
    {
      switch (volume->device_type)
        {
        case DEVICE_TYPE_UNKNOWN:
        case DEVICE_TYPE_DISK:
            volume->fs_type = strdup ("auto");
          break;
        case DEVICE_TYPE_CDROM:
            volume->fs_type = strdup ("udf,iso9660");
          break;
        default:
          assert (FALSE);
          break;
        }
    }

  return volume;
}

static void
volume_free (Volume *volume)
{

  if (!volume)
    return;

  if (volume->udi != NULL)
    {
      free (volume->udi);
      volume->udi = NULL;
    }

  if (volume->block_device != NULL)
    {
      free (volume->block_device);
      volume->block_device = NULL;
    }

  if (volume->fs_type != NULL)
    {
      free (volume->fs_type);
      volume->fs_type = NULL;
    }

  if (volume->type != NULL)
    {
      free (volume->type);
      volume->type = NULL;
    }
}

/* FIXME: This function should be more fleshed out then it is
 */
static boolean
create_mount_point_for_volume (Volume *volume)
{

  /* FIXME: Should only mkdir if we need to and should do so
   * recursively for each component of the mount root.
   */
  mkdir (MOUNT_ROOT, 0775);

  return (mkdir (volume->mount_point, 0775) != -1) || errno == EEXIST;
}

static boolean
fs_table_has_block_device (FSTable *table, const char *block_device)
{
  FSTableLine *line;

  line = table->lines;
  while (line != NULL)
    {
      FSTableField *field;

      field = line->fields;
      while (field != NULL)
        {
          if (field->type == FS_TABLE_FIELD_TYPE_BLOCK_DEVICE)
            {
              if (strcmp (field->value, block_device) == 0)
                return TRUE;

              break;
            }
          field = field->next;
        }
      line = line->next;
    }

  return FALSE;
}

static boolean
fs_table_add_volume (FSTable *table, Volume *volume)
{
  char *mount_options;
  FSTableLine *line;

  if (fs_table_has_block_device (table, volume->block_device))
    {
      fstab_update_debug (_("Could not add entry to fstab file: "
                            "block device already listed\n"));
      return FALSE;
    }

  switch (volume->device_type)
    {
    case DEVICE_TYPE_UNKNOWN:
    case DEVICE_TYPE_DISK:
#ifdef USE_NOOP_MOUNT_OPTION
      mount_options = "noauto,user,exec,dev,suid,kudzu";
#else
      mount_options = "noauto,user,exec,dev,suid";
#endif
      break;
    case DEVICE_TYPE_CDROM:
#ifdef USE_NOOP_MOUNT_OPTION
      mount_options = "noauto,user,exec,dev,suid,kudzu,ro";
#else
      mount_options = "noauto,user,exec,dev,suid,ro";
#endif
      break;
    default:
      assert (FALSE);
      break;
    }

  line = fs_table_line_new_from_field_values (volume->block_device,
                                              volume->mount_point,
                                              volume->fs_type,
                                              mount_options, 0, 0); 
  fs_table_add_line (table, line);

  return TRUE;
}

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
fs_table_remove_volume (FSTable *table, Volume *volume)
{
  FSTableLine *line, *previous_line; 

  previous_line = NULL;
  line = table->lines;
  while (line != NULL)
    {
      if (line->block_device != NULL
          && strcmp (line->block_device, volume->block_device) == 0
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
      fstab_update_debug (_("Could not stat '%s': %s\n"),
                          filename, strerror (errno));
    }
  
  return buf.st_mtime;
}

static int
open_and_lock_file (const char *filename)
{
  struct flock lock;
  int fd, saved_errno, lock_status;
  time_t timeout;

  fd = open (filename, O_RDONLY);

  if (fd < 0)
    {
      fstab_update_debug (_("failed to open '%s': %s\n"),
                          filename, strerror (errno));
      return FALSE;
    }

  lock.l_type = F_RDLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = 0;
  lock.l_len = 0;

  timeout = time (NULL) + LOCK_TIMEOUT;

  while ((lock_status = fcntl (fd, F_SETLK, &lock)) < 0)
    {
      saved_errno = errno;

      if (saved_errno != EACCES 
          && saved_errno != EAGAIN 
          && saved_errno != EINTR)
        break;

      usleep (LOCK_TIMEOUT_WAIT);

      if (time (NULL) > timeout)
        {
          fstab_update_debug (_("timed out waiting for read lock on '%s'\n"),
                              filename);
          close (fd);
          return FALSE;
        }
    }

  if (lock_status < 0)
    {
      fstab_update_debug (_("Could not get read lock on '%s': %s\n"),
                          filename, strerror (saved_errno));
      close (fd);
      return FALSE;
    }

  return fd;
}

static boolean
volume_determine_mount_point (Volume *volume, FSTable *table)
{
  FSTableLine *line;
  char *device_type;
  unsigned long device_number, next_available_device_number = 0;
  long length;
  struct stat buf;

  for (line = table->lines; line != NULL; line = line->next)
    {
      char *p = NULL, *q = NULL, *end = NULL, *mount_root = NULL;

      if (line->mount_point == NULL)
        continue;

      p = strrchr (line->mount_point, '/');

      if (p != NULL)
        {
          mount_root = strndup (line->mount_point, p - line->mount_point + 1);
          p++;
        }
      else
        continue;

      if (strcmp (mount_root, MOUNT_ROOT) != 0)
        {
          free (mount_root);
          continue;
        }
      free (mount_root);

      for (q = p; *q != '\0' && !isspace (*q) && !isdigit (*q); q++);

      device_type = strndup (p, q - p);

      /* If the mount point doesn't end in a number, assume an implied 0 
       */
      if (*q == '\0' || !isdigit (*q))
        device_number = 0;
      else
        device_number = strtoul (q, &end, 10);

      assert (q != end);

      /* Generated mount points don't have numbers in the middle of them--
       * just at the end
       */
      if (end != NULL && *end != '\0')
        {
          free (device_type);
          continue;
        }

      if (strcmp (volume->type, device_type) == 0
          && next_available_device_number == device_number)
        next_available_device_number++;
    }

  length = strlen (volume->type) + sizeof (MOUNT_ROOT) + 6 /* digits */;
  volume->mount_point = malloc (length);

  if (next_available_device_number == 0)
    {
      strcpy (volume->mount_point, MOUNT_ROOT);
      strcat (volume->mount_point, volume->type);
    }
  else if (snprintf (volume->mount_point, length, MOUNT_ROOT"%s%lu",
                     volume->type, next_available_device_number) > length)
    {
      fstab_update_debug (_("Could not use mount point '%s': %s\n"),
                          volume->mount_point, "too long");

      free (volume->mount_point);
      volume->mount_point = NULL;
      return FALSE;
    }

  if (stat (volume->mount_point, &buf) < 0 && errno != ENOENT)
    {
      fstab_update_debug (_("Could not use mount point '%s': %s\n"),
                          volume->mount_point, strerror (errno));

      free (volume->mount_point);
      volume->mount_point = NULL;
      return FALSE;
    }

  return TRUE;
}

static boolean
add_udi (const char *udi, boolean should_mount_device)
{
  Volume *volume;
  FSTable *fs_table;
  char *dir, *last_slash, *temp_filename = NULL;
  time_t fstab_modification_time;
  int fd = -1;

  volume = volume_new (udi);

  if (volume == NULL)
    return FALSE;

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

  fstab_modification_time = get_file_modification_time (_PATH_FSTAB);

  if (fstab_modification_time == 0)
    goto error;

  if (!volume_determine_mount_point (volume, fs_table))
    goto error;

  if (!fs_table_add_volume (fs_table, volume))
    goto error;

  if (!fs_table_write (fs_table, fd))
    goto error;

  /* Someone changed the fs table under us, better start over.
   */
  if (get_file_modification_time (_PATH_FSTAB) != fstab_modification_time)
    {
      close (fd);
      unlink (temp_filename);
      volume_free (volume);
      return add_udi (udi, should_mount_device);
    }

  if (rename (temp_filename, _PATH_FSTAB) < 0)
    {
      fstab_update_debug (_("Failed to rename '%s' to '%s': %s\n"),
                          temp_filename, _PATH_FSTAB, strerror (errno));
      goto error;
    }

  if (!create_mount_point_for_volume (volume))
    goto error;

  if (should_mount_device && !mount_device (volume->mount_point))
    goto error;

  close (fd);
  volume_free (volume);

  return TRUE;

error:
  if (fd >= 0)
    close (fd);
  volume_free (volume);
  if (temp_filename != NULL)
    unlink (temp_filename);
  return FALSE;
}

static boolean
remove_udi (const char *udi)
{
  Volume *volume;
  FSTable *fs_table = NULL;
  FSTableLine *line = NULL;
  char *dir, *last_slash, *temp_filename = NULL;
  time_t fstab_modification_time;
  int fd;

  volume = volume_new (udi);

  if (volume == NULL)
    return FALSE;

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

  line = fs_table_remove_volume (fs_table, volume);

  if (line == NULL)
    {
      fstab_update_debug (_("Could not remove device '%s' with UDI '%s' from "
                            "fs table: not found\n"),
                          volume->block_device, udi);
      goto error;
    }

  assert (line->mount_point != NULL);

  if (rmdir (line->mount_point) < 0)
    {
      fstab_update_debug (_("Failed to remove mount point '%s': %s\n"),
                          line->mount_point, strerror (errno));
      goto error;
    }

  fs_table_line_free (line);

  if (!fs_table_write (fs_table, fd))
    goto error;

  /* Someone changed the fs table under us, better start over.
   */
  if (get_file_modification_time (_PATH_FSTAB) != fstab_modification_time)
    {
      close (fd);
      unlink (temp_filename);
      volume_free (volume);
      return remove_udi (udi);
    }

  if (rename (temp_filename, _PATH_FSTAB) < 0)
    {
      fstab_update_debug (_("Failed to rename '%s' to '%s': %s\n"),
                          temp_filename, _PATH_FSTAB, strerror (errno));
      goto error;
    }

  close (fd);
  volume_free (volume);

  return TRUE;

error:
  if (fd >= 0)
    close (fd);

  volume_free (volume);

  if (temp_filename != NULL)
    unlink (temp_filename);

  fs_table_line_free (line);

  return FALSE;
}

static void
fs_table_remove_generated_entries (FSTable *table)
{
  FSTableLine *line, *previous_line; 

  previous_line = NULL;
  line = table->lines;
  while (line != NULL)
    {
      if (fs_table_line_is_generated (line))
        {
          if (previous_line == NULL)
              table->lines = line->next;
          else 
            {
              if (line->next == NULL)
                table->tail = previous_line;
              previous_line->next = line->next;
            }
        }

      previous_line = line;
      line = line->next;
    }
}

static boolean
clean (void)
{
  FSTable *fs_table;
  char *dir, *last_slash, *temp_filename = NULL;
  time_t fstab_modification_time;
  int fd;

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
      fstab_update_debug (_("Failed to rename '%s' to '%s': %s\n"),
                          temp_filename, _PATH_FSTAB, strerror (errno));
      goto error;
    }

  close (fd);

  return TRUE;

error:
  if (fd >= 0)
    close (fd);
  if (temp_filename != NULL)
    unlink (temp_filename);
  return FALSE;
}

int
main (int argc, const char *argv[])
{
  int i, retval = 0;
  poptContext popt_context;
  boolean should_clean = FALSE, should_mount_device = FALSE;
  char *udi_to_add = NULL, *udi_to_remove = NULL;

  struct poptOption options[] = {
      {"add", 'a', POPT_ARG_STRING, &udi_to_add, 0,
        N_("add an entry to fstab"), N_("UDI")},
      {"mount", 'm', POPT_ARG_NONE, &should_mount_device, 0,
        N_("Mount added entry")},
      {"remove", 'r', POPT_ARG_STRING, &udi_to_remove, 0,
        N_("remove an entry from fstab"), N_("UDI")},
      {"clean", 'c', POPT_ARG_NONE, &should_clean, 0,
        N_("Remove all generated entries from fstab")},
      {"verbose", 'v', POPT_ARG_NONE, &verbose, 0,
        N_("Report detailed information about operation progress")},

      POPT_AUTOHELP

      {NULL, '\0', 0, NULL},
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
  poptFreeContext (popt_context);

  if (udi_to_add || udi_to_remove)
    {
      LibHalFunctions halFunctions = { NULL };

      hal_context = hal_initialize (&halFunctions, 0);

      if (udi_to_add)
        retval = !add_udi (udi_to_add, should_mount_device);

      if (udi_to_remove)
        retval |= !remove_udi (udi_to_remove);

      hal_shutdown (hal_context);
    }
  else if (should_clean)
    retval = clean ();
  else
    {
      poptPrintHelp (popt_context, stderr, 0);
      return 1;
    }

  return retval;
}
