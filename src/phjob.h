/*

    File: phjob.h

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
#ifndef _PHJOB_H
#define _PHJOB_H
#ifdef __cplusplus
extern "C" {
#endif

#include "photorec.h"
#include "filegen.h"

/*
 * Global flag: when non-zero, all interactive prompts are suppressed.
 * Set by /job or /batch command-line flag.
 */
extern int photorec_batch_mode;

/*
 * Load a job profile from an INI-style configuration file.
 *
 * The file format is:
 *
 *   # Comment lines start with # or ;
 *   [device]
 *   path = /dev/mapper/system-root
 *
 *   [scan]
 *   mode = freespace          # freespace | wholespace
 *   partition_type = None     # None | Intel | EFI GPT | Mac | Sun
 *   blocksize = 0             # 0 = auto-detect
 *
 *   [filetypes]
 *   default = disable         # enable | disable  (sets initial state for all types)
 *   zip = enable
 *   jpg = enable
 *   doc = enable
 *
 *   [options]
 *   paranoid = 1              # 0=none, 1=validate, 2=brute-force
 *   keep_corrupted_file = 0   # 0=discard, 1=keep
 *   expert = 0
 *   lowmem = 0
 *
 *   [output]
 *   directory = /mnt/ssd/recovery
 *
 * Returns 0 on success, -1 on error.
 *
 * On success, params->cmd_device, params->cmd_run, params->recup_dir
 * and options fields are populated.  The caller owns all allocated memory
 * (cmd_run is malloc'd, caller must free).
 */
int photorec_load_job(const char *job_file,
    struct ph_param *params, struct ph_options *options);

#ifdef __cplusplus
} /* closing brace for extern "C" */
#endif
#endif
