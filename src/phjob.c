/*

    File: phjob.c

    Copyright (C) 2026 Stephane Denis <stephane@sdenis.com>

    PhotoRec job profile: run a fully non-interactive recovery from a
    configuration file.

    This software is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write the Free Software Foundation, Inc., 51
    Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <ctype.h>
#include "types.h"
#include "common.h"
#include "filegen.h"
#include "photorec.h"
#include "phjob.h"
#include "log.h"

/* Batch mode flag — when set, all interactive prompts are suppressed */
int photorec_batch_mode = 0;

/* Maximum line length in the config file */
#define MAX_LINE 1024

/* Maximum length of the generated command string */
#define MAX_CMD  8192

/* Trim leading and trailing whitespace in-place */
static char *trim(char *s)
{
  char *end;
  while(*s && isspace((unsigned char)*s))
    s++;
  if(*s == '\0')
    return s;
  end = s + strlen(s) - 1;
  while(end > s && isspace((unsigned char)*end))
    *end-- = '\0';
  return s;
}

/* Check if a line is a comment or empty */
static int is_comment_or_empty(const char *line)
{
  const char *p = line;
  while(*p && isspace((unsigned char)*p))
    p++;
  return (*p == '\0' || *p == '#' || *p == ';');
}

/* Check if a line is a section header [section], return section name */
static int is_section(const char *line, char *section, size_t section_size)
{
  const char *p = line;
  const char *end;
  size_t len;

  while(*p && isspace((unsigned char)*p))
    p++;
  if(*p != '[')
    return 0;
  p++;
  end = strchr(p, ']');
  if(end == NULL)
    return 0;
  len = (size_t)(end - p);
  if(len >= section_size)
    len = section_size - 1;
  memcpy(section, p, len);
  section[len] = '\0';
  return 1;
}

/* Parse a "key = value" line, return 1 if successful */
static int parse_key_value(const char *line, char *key, size_t key_size,
    char *value, size_t value_size)
{
  const char *eq;
  const char *p;
  size_t len;

  eq = strchr(line, '=');
  if(eq == NULL)
    return 0;

  /* Key: everything before '=' */
  len = (size_t)(eq - line);
  if(len >= key_size)
    len = key_size - 1;
  memcpy(key, line, len);
  key[len] = '\0';
  /* Trim key */
  {
    char *trimmed = trim(key);
    if(trimmed != key)
      memmove(key, trimmed, strlen(trimmed) + 1);
  }

  /* Value: everything after '=' */
  p = eq + 1;
  while(*p && isspace((unsigned char)*p))
    p++;

  /* Strip inline comment (# or ;) but not inside quotes */
  {
    const char *comment = p;
    while(*comment && *comment != '#' && *comment != ';')
      comment++;
    len = (size_t)(comment - p);
  }
  if(len >= value_size)
    len = value_size - 1;
  memcpy(value, p, len);
  value[len] = '\0';
  /* Trim value */
  {
    char *trimmed = trim(value);
    if(trimmed != value)
      memmove(value, trimmed, strlen(trimmed) + 1);
  }

  return 1;
}

/* Append to a command string with comma separator */
static void cmd_append(char *cmd, size_t cmd_size, const char *fragment)
{
  size_t len = strlen(cmd);
  if(len > 0 && len < cmd_size - 1)
  {
    cmd[len] = ',';
    cmd[len + 1] = '\0';
    len++;
  }
  if(len + strlen(fragment) < cmd_size)
    strcat(cmd, fragment);
}

int photorec_load_job(const char *job_file,
    struct ph_param *params, struct ph_options *options)
{
  FILE *f;
  char line[MAX_LINE];
  char section[64] = "";
  char key[128], value[512];

  /* Temporary storage for config values */
  char device_path[512] = "";
  char partition_type[64] = "";
  char scan_mode[64] = "";
  char output_dir[512] = "";
  unsigned int blocksize = 0;
  int paranoid = -1;          /* -1 = not set, use default */
  int keep_corrupted = -1;
  int expert = -1;
  int lowmem = -1;

  /* File type overrides: we build a mini command string */
  char filetype_cmd[MAX_CMD] = "";
  int has_filetypes = 0;

  f = fopen(job_file, "r");
  if(f == NULL)
  {
    log_error("phjob: cannot open job file '%s'\n", job_file);
    printf("Error: cannot open job file '%s'\n", job_file);
    return -1;
  }

  log_info("phjob: loading job profile from '%s'\n", job_file);

  while(fgets(line, sizeof(line), f) != NULL)
  {
    char *trimmed = trim(line);

    if(is_comment_or_empty(trimmed))
      continue;

    if(is_section(trimmed, section, sizeof(section)))
      continue;

    if(!parse_key_value(trimmed, key, sizeof(key), value, sizeof(value)))
      continue;

    /* [device] section */
    if(strcmp(section, "device") == 0)
    {
      if(strcmp(key, "path") == 0)
        strncpy(device_path, value, sizeof(device_path) - 1);
    }
    /* [scan] section */
    else if(strcmp(section, "scan") == 0)
    {
      if(strcmp(key, "mode") == 0)
        strncpy(scan_mode, value, sizeof(scan_mode) - 1);
      else if(strcmp(key, "partition_type") == 0)
        strncpy(partition_type, value, sizeof(partition_type) - 1);
      else if(strcmp(key, "blocksize") == 0)
        blocksize = (unsigned int)atoi(value);
    }
    /* [filetypes] section */
    else if(strcmp(section, "filetypes") == 0)
    {
      if(strcmp(key, "default") == 0)
      {
        /* "enable" or "disable" all types first */
        if(strcmp(value, "disable") == 0 || strcmp(value, "enable") == 0)
        {
          if(has_filetypes)
            cmd_append(filetype_cmd, sizeof(filetype_cmd), "");
          char tmp[64];
          snprintf(tmp, sizeof(tmp), "everything,%s", value);
          cmd_append(filetype_cmd, sizeof(filetype_cmd), tmp);
          has_filetypes = 1;
        }
      }
      else
      {
        /* Individual file type: key = extension, value = enable/disable */
        if(strcmp(value, "enable") == 0 || strcmp(value, "disable") == 0)
        {
          char tmp[128];
          snprintf(tmp, sizeof(tmp), "%s,%s", key, value);
          cmd_append(filetype_cmd, sizeof(filetype_cmd), tmp);
          has_filetypes = 1;
        }
      }
    }
    /* [options] section */
    else if(strcmp(section, "options") == 0)
    {
      if(strcmp(key, "paranoid") == 0)
        paranoid = atoi(value);
      else if(strcmp(key, "keep_corrupted_file") == 0)
        keep_corrupted = atoi(value);
      else if(strcmp(key, "expert") == 0)
        expert = atoi(value);
      else if(strcmp(key, "lowmem") == 0)
        lowmem = atoi(value);
    }
    /* [output] section */
    else if(strcmp(section, "output") == 0)
    {
      if(strcmp(key, "directory") == 0)
        strncpy(output_dir, value, sizeof(output_dir) - 1);
    }
    else
    {
      log_warning("phjob: unknown section [%s], key '%s'\n", section, key);
    }
  }
  fclose(f);

  /* Validate required fields */
  if(device_path[0] == '\0')
  {
    log_error("phjob: [device] path is required\n");
    printf("Error: job file missing [device] path\n");
    return -1;
  }

  /* Apply options directly */
  if(paranoid >= 0)
    options->paranoid = paranoid;
  if(keep_corrupted >= 0)
    options->keep_corrupted_file = keep_corrupted;
  if(expert >= 0)
    options->expert = (unsigned int)expert;
  if(lowmem >= 0)
    options->lowmem = (unsigned int)lowmem;

  /* Build the /cmd command string */
  {
    char *cmd = (char *)malloc(MAX_CMD);
    if(cmd == NULL)
      return -1;
    cmd[0] = '\0';

    /* Partition type (first in the command, consumed by change_arch_type_cli) */
    if(partition_type[0] != '\0')
      cmd_append(cmd, MAX_CMD, partition_type);

    /* File type options */
    if(has_filetypes)
    {
      cmd_append(cmd, MAX_CMD, "fileopt");
      strcat(cmd, ",");
      strncat(cmd, filetype_cmd, MAX_CMD - strlen(cmd) - 1);
    }

    /* Options sub-commands */
    if(paranoid >= 0 || keep_corrupted >= 0 || expert >= 0 || lowmem >= 0)
    {
      cmd_append(cmd, MAX_CMD, "options");
      if(paranoid == 0)
        cmd_append(cmd, MAX_CMD, "paranoid_no");
      else if(paranoid == 1)
        cmd_append(cmd, MAX_CMD, "paranoid");
      else if(paranoid == 2)
        cmd_append(cmd, MAX_CMD, "paranoid_bf");

      if(keep_corrupted == 1)
        cmd_append(cmd, MAX_CMD, "keep_corrupted_file");
      else if(keep_corrupted == 0)
        cmd_append(cmd, MAX_CMD, "keep_corrupted_file_no");
    }

    /* Blocksize */
    if(blocksize > 0)
    {
      char tmp[64];
      snprintf(tmp, sizeof(tmp), "blocksize,%u", blocksize);
      cmd_append(cmd, MAX_CMD, tmp);
    }

    /* Scan mode: freespace or wholespace */
    if(strcmp(scan_mode, "freespace") == 0 || strcmp(scan_mode, "free") == 0)
      cmd_append(cmd, MAX_CMD, "freespace");
    else if(strcmp(scan_mode, "wholespace") == 0 || strcmp(scan_mode, "whole") == 0)
      cmd_append(cmd, MAX_CMD, "wholespace");
    else if(scan_mode[0] == '\0')
      cmd_append(cmd, MAX_CMD, "freespace");  /* Default: free space only */
    else
    {
      log_warning("phjob: unknown scan mode '%s', defaulting to freespace\n", scan_mode);
      cmd_append(cmd, MAX_CMD, "freespace");
    }

    /* The search command triggers the actual scan */
    cmd_append(cmd, MAX_CMD, "search");

    /* Store the device */
    params->cmd_device = strdup(device_path);

    /* Store the command string */
    params->cmd_run = cmd;

    /* Output directory */
    if(output_dir[0] != '\0')
    {
      free(params->recup_dir);
      params->recup_dir = strdup(output_dir);
    }
  }

  /* Enable batch mode — suppress all interactive prompts */
  photorec_batch_mode = 1;

  log_info("phjob: device     = %s\n", params->cmd_device);
  log_info("phjob: cmd_run    = %s\n", params->cmd_run);
  log_info("phjob: output_dir = %s\n", params->recup_dir ? params->recup_dir : "(not set)");
  log_info("phjob: batch_mode = %d\n", photorec_batch_mode);

  return 0;
}
