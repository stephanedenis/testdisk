/*

    File: btrfsp.c

    Copyright (C) 2026 Stephane Denis <stephane@sdenis.com>

    PhotoRec: Exclude allocated btrfs blocks from search space.

    This reads the btrfs superblock and chunk tree to identify which
    byte ranges on disk are allocated to data/metadata chunks. Those
    ranges are removed from the PhotoRec search space so that only
    unallocated (free) regions are scanned for deleted files.

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
#include "types.h"
#include "common.h"
#include "list.h"
#include "filegen.h"
#include "photorec.h"
#include "btrfs.h"
#include "btrfsp.h"
#include "log.h"

/*
 * Btrfs on-disk structures needed to parse the chunk tree.
 * We define them here to avoid depending on external btrfs headers.
 */

/* Key types */
#define BTRFS_CHUNK_ITEM_KEY    228
#define BTRFS_DEV_EXTENT_KEY    204
#define BTRFS_BLOCK_GROUP_ITEM_KEY 192

/* Chunk type flags */
#define BTRFS_BLOCK_GROUP_DATA     (1ULL << 0)
#define BTRFS_BLOCK_GROUP_SYSTEM   (1ULL << 1)
#define BTRFS_BLOCK_GROUP_METADATA (1ULL << 2)

/* On-disk key */
struct btrfs_disk_key {
  uint64_t objectid;
  uint8_t  type;
  uint64_t offset;
} __attribute__ ((gcc_struct, __packed__));

/* Chunk item stripe */
struct btrfs_stripe {
  uint64_t devid;
  uint64_t offset;      /* physical offset on device */
  uint8_t  dev_uuid[BTRFS_UUID_SIZE];
} __attribute__ ((gcc_struct, __packed__));

/* Chunk item */
struct btrfs_chunk {
  uint64_t length;      /* size of this chunk in bytes */
  uint64_t owner;       /* objectid of the root referencing this chunk */
  uint64_t stripe_len;
  uint64_t type;        /* BTRFS_BLOCK_GROUP_DATA | METADATA | SYSTEM */
  uint32_t io_align;
  uint32_t io_width;
  uint32_t sector_size;
  uint16_t num_stripes;
  uint16_t sub_stripes;
  struct btrfs_stripe stripe;
  /* additional stripes follow */
} __attribute__ ((gcc_struct, __packed__));

/* B-tree node header */
struct btrfs_header {
  uint8_t  csum[BTRFS_CSUM_SIZE];
  uint8_t  fsid[BTRFS_FSID_SIZE];
  uint64_t bytenr;
  uint64_t flags;
  uint8_t  chunk_tree_uuid[BTRFS_UUID_SIZE];
  uint64_t generation;
  uint64_t owner;
  uint32_t nritems;
  uint8_t  level;
} __attribute__ ((gcc_struct, __packed__));

/* Leaf item (in a leaf node, level==0) */
struct btrfs_item {
  struct btrfs_disk_key key;
  uint32_t offset;  /* offset from end of header to the data */
  uint32_t size;    /* size of the data */
} __attribute__ ((gcc_struct, __packed__));

/* Internal node key pointer (in internal nodes, level>0) */
struct btrfs_key_ptr {
  struct btrfs_disk_key key;
  uint64_t blockptr;
  uint64_t generation;
} __attribute__ ((gcc_struct, __packed__));

/* Sys chunk array entry: key + chunk */
struct btrfs_sys_chunk {
  struct btrfs_disk_key key;
  struct btrfs_chunk chunk;
} __attribute__ ((gcc_struct, __packed__));

/* Block group item */
struct btrfs_block_group_item {
  uint64_t used;
  uint64_t chunk_objectid;
  uint64_t flags;
} __attribute__ ((gcc_struct, __packed__));

/*
 * Remove allocated chunk ranges from PhotoRec search space.
 *
 * Strategy:
 * 1. Read the btrfs superblock to get sectorsize and sys_chunk_array
 * 2. Parse sys_chunk_array to get system chunk physical locations
 * 3. Read the chunk tree root (from superblock->chunk_root) 
 * 4. Walk the chunk tree leaf nodes to find all CHUNK_ITEM entries
 * 5. For each chunk: its stripes tell us the physical byte ranges on disk
 * 6. Call del_search_space() for each physical range that is allocated
 *
 * This excludes all currently-allocated data, metadata, and system chunks,
 * leaving only the genuinely free space to be scanned for deleted files.
 */

/* Maximum number of chunks we track */
#define MAX_CHUNKS 65536

struct chunk_info {
  uint64_t physical;   /* physical offset on this device */
  uint64_t length;     /* length of the chunk */
  uint64_t type;       /* chunk type flags */
};

static unsigned int chunk_count = 0;
static struct chunk_info chunks[MAX_CHUNKS];

static void add_chunk(const uint64_t physical, const uint64_t length, const uint64_t type)
{
  if(chunk_count >= MAX_CHUNKS)
  {
    log_warning("btrfs_remove_used_space: too many chunks (>%u), some allocated space may not be excluded\n", MAX_CHUNKS);
    return;
  }
  chunks[chunk_count].physical = physical;
  chunks[chunk_count].length = length;
  chunks[chunk_count].type = type;
  chunk_count++;
}

/*
 * Parse the sys_chunk_array embedded in the superblock.
 * This gives us the system chunks (needed to bootstrap reading the chunk tree).
 */
static void parse_sys_chunk_array(const struct btrfs_super_block *sb)
{
  const uint8_t *array = sb->sys_chunk_array;
  const uint32_t array_size = le32(sb->sys_chunk_array_size);
  uint32_t offset = 0;

  log_trace("btrfs: parsing sys_chunk_array, size=%u\n", array_size);

  while(offset < array_size)
  {
    const struct btrfs_disk_key *key;
    const struct btrfs_chunk *chunk;
    uint16_t num_stripes;
    uint16_t i;

    if(offset + sizeof(struct btrfs_disk_key) > array_size)
      break;

    key = (const struct btrfs_disk_key *)(array + offset);
    offset += sizeof(struct btrfs_disk_key);

    if(key->type != BTRFS_CHUNK_ITEM_KEY)
    {
      log_warning("btrfs: unexpected key type %u in sys_chunk_array\n", key->type);
      break;
    }

    if(offset + sizeof(struct btrfs_chunk) > array_size)
      break;

    chunk = (const struct btrfs_chunk *)(array + offset);
    num_stripes = le16(chunk->num_stripes);

    log_trace("btrfs: sys_chunk logical=%llu length=%llu type=0x%llx stripes=%u\n",
        (unsigned long long)le64(key->offset),
        (unsigned long long)le64(chunk->length),
        (unsigned long long)le64(chunk->type),
        num_stripes);

    /* Add each stripe's physical location */
    if(num_stripes > 0)
    {
      /* First stripe is embedded in the chunk struct */
      add_chunk(le64(chunk->stripe.offset), le64(chunk->length), le64(chunk->type));

      /* Additional stripes follow the chunk struct */
      for(i = 1; i < num_stripes; i++)
      {
        const struct btrfs_stripe *extra_stripe;
        const uint32_t stripe_offset = offset + sizeof(struct btrfs_chunk) +
          (uint32_t)(i - 1) * sizeof(struct btrfs_stripe);
        if(stripe_offset + sizeof(struct btrfs_stripe) > array_size)
          break;
        extra_stripe = (const struct btrfs_stripe *)(array + stripe_offset);
        add_chunk(le64(extra_stripe->offset), le64(chunk->length), le64(chunk->type));
      }
    }

    /* Advance past the chunk struct and all its stripes */
    offset += sizeof(struct btrfs_chunk) +
      (uint32_t)(num_stripes > 0 ? num_stripes - 1 : 0) * sizeof(struct btrfs_stripe);
  }
}

/*
 * Parse a single leaf node of the chunk tree.
 * Extract all CHUNK_ITEM entries and add their physical stripes.
 */
static void parse_chunk_tree_leaf(
    const unsigned char *leaf_buf, const uint32_t nodesize)
{
  const struct btrfs_header *header = (const struct btrfs_header *)leaf_buf;
  const uint32_t nritems = le32(header->nritems);
  uint32_t i;

  if(header->level != 0)
    return;  /* Not a leaf */

  for(i = 0; i < nritems; i++)
  {
    const struct btrfs_item *item;
    const uint32_t item_offset_in_buf = sizeof(struct btrfs_header) +
      (uint32_t)i * sizeof(struct btrfs_item);

    if(item_offset_in_buf + sizeof(struct btrfs_item) > nodesize)
      break;

    item = (const struct btrfs_item *)(leaf_buf + item_offset_in_buf);

    if(item->key.type == BTRFS_CHUNK_ITEM_KEY)
    {
      const uint32_t data_offset = sizeof(struct btrfs_header) +
        le32(item->offset);
      const struct btrfs_chunk *chunk;
      uint16_t num_stripes;
      uint16_t s;

      if(data_offset + sizeof(struct btrfs_chunk) > nodesize)
        continue;

      chunk = (const struct btrfs_chunk *)(leaf_buf + data_offset);
      num_stripes = le16(chunk->num_stripes);

      log_trace("btrfs: chunk logical=%llu length=%llu type=0x%llx stripes=%u\n",
          (unsigned long long)le64(item->key.offset),
          (unsigned long long)le64(chunk->length),
          (unsigned long long)le64(chunk->type),
          num_stripes);

      /* First stripe */
      if(num_stripes > 0)
      {
        add_chunk(le64(chunk->stripe.offset), le64(chunk->length), le64(chunk->type));
      }

      /* Additional stripes */
      for(s = 1; s < num_stripes; s++)
      {
        const uint32_t stripe_off = data_offset + sizeof(struct btrfs_chunk) +
          (uint32_t)(s - 1) * sizeof(struct btrfs_stripe);
        const struct btrfs_stripe *extra;

        if(stripe_off + sizeof(struct btrfs_stripe) > nodesize)
          break;

        extra = (const struct btrfs_stripe *)(leaf_buf + stripe_off);
        add_chunk(le64(extra->offset), le64(chunk->length), le64(chunk->type));
      }
    }
  }
}

/*
 * Recursively walk the chunk tree from a given node.
 * - If level==0 (leaf): parse chunk items
 * - If level>0 (internal): read child nodes and recurse
 * max_depth prevents runaway recursion on corrupt trees.
 */
static void walk_chunk_tree(disk_t *disk, const partition_t *partition,
    const uint64_t node_bytenr, const uint32_t nodesize, const int max_depth)
{
  unsigned char *buf;
  const struct btrfs_header *header;

  if(max_depth <= 0)
    return;

  buf = (unsigned char *)MALLOC(nodesize);
  if(buf == NULL)
    return;

  /* Read the node from disk.
   * For single-device btrfs, logical == physical on the device.
   * The node_bytenr is relative to the start of the btrfs filesystem,
   * so we add the partition offset. */
  if(disk->pread(disk, buf, nodesize, partition->part_offset + node_bytenr) != (int)nodesize)
  {
    log_warning("btrfs: failed to read chunk tree node at offset %llu\n",
        (unsigned long long)node_bytenr);
    free(buf);
    return;
  }

  header = (const struct btrfs_header *)buf;

  /* Validate: check that the header's bytenr matches where we read it */
  if(le64(header->bytenr) != partition->part_offset + node_bytenr &&
     le64(header->bytenr) != node_bytenr)
  {
    /* On single-device, bytenr in the header is the logical address.
     * For single device, logical addr == physical addr relative to
     * device start. So header->bytenr should equal node_bytenr. */
    log_warning("btrfs: chunk tree node bytenr mismatch: expected %llu, got %llu\n",
        (unsigned long long)node_bytenr,
        (unsigned long long)le64(header->bytenr));
    /* Try to continue anyway - the data might still be valid */
  }

  if(header->level == 0)
  {
    /* Leaf node: parse chunk items */
    parse_chunk_tree_leaf(buf, nodesize);
  }
  else
  {
    /* Internal node: follow child pointers */
    const uint32_t nritems = le32(header->nritems);
    uint32_t i;

    for(i = 0; i < nritems; i++)
    {
      const uint32_t kp_offset = sizeof(struct btrfs_header) +
        (uint32_t)i * sizeof(struct btrfs_key_ptr);
      const struct btrfs_key_ptr *kp;

      if(kp_offset + sizeof(struct btrfs_key_ptr) > nodesize)
        break;

      kp = (const struct btrfs_key_ptr *)(buf + kp_offset);
      walk_chunk_tree(disk, partition, le64(kp->blockptr), nodesize, max_depth - 1);
    }
  }

  free(buf);
}

unsigned int btrfs_remove_used_space(disk_t *disk, const partition_t *partition, alloc_data_t *list_search_space)
{
  struct btrfs_super_block *sb;
  unsigned char *sb_buf;
  uint32_t sectorsize;
  uint32_t nodesize;
  uint64_t chunk_root;
  unsigned int i;
  uint64_t total_excluded = 0;
  uint64_t start_free = 0;
  uint64_t end_free = 0;

  /* Reset chunk tracking */
  chunk_count = 0;

  /* Read the superblock */
  sb_buf = (unsigned char *)MALLOC(BTRFS_SUPER_INFO_SIZE);
  if(sb_buf == NULL)
    return 0;

  if(disk->pread(disk, sb_buf, BTRFS_SUPER_INFO_SIZE,
        partition->part_offset + BTRFS_SUPER_INFO_OFFSET) != BTRFS_SUPER_INFO_SIZE)
  {
    log_error("btrfs_remove_used_space: failed to read superblock\n");
    free(sb_buf);
    return 0;
  }

  sb = (struct btrfs_super_block *)sb_buf;

  /* Validate magic */
  if(memcmp(&sb->magic, BTRFS_MAGIC, 8) != 0)
  {
    log_error("btrfs_remove_used_space: invalid btrfs magic\n");
    free(sb_buf);
    return 0;
  }

  sectorsize = le32(sb->sectorsize);
  nodesize = le32(sb->nodesize);
  chunk_root = le64(sb->chunk_root);

  if(sectorsize == 0 || nodesize == 0)
  {
    log_error("btrfs_remove_used_space: invalid sectorsize=%u or nodesize=%u\n",
        sectorsize, nodesize);
    free(sb_buf);
    return 0;
  }

  log_info("btrfs_remove_used_space: sectorsize=%u nodesize=%u chunk_root=%llu\n",
      sectorsize, nodesize, (unsigned long long)chunk_root);
  log_info("btrfs_remove_used_space: total_bytes=%llu bytes_used=%llu\n",
      (unsigned long long)le64(sb->total_bytes),
      (unsigned long long)le64(sb->bytes_used));

  /* Step 1: Parse sys_chunk_array from superblock for bootstrap chunks */
  parse_sys_chunk_array(sb);

  log_info("btrfs_remove_used_space: %u chunks from sys_chunk_array\n", chunk_count);

  /* Step 2: Walk the chunk tree to discover all chunk mappings.
   * The chunk_root logical address should be resolvable via sys_chunk_array
   * (system chunks). For single-device btrfs, logical == physical. */
  walk_chunk_tree(disk, partition, chunk_root, nodesize, 8);

  log_info("btrfs_remove_used_space: %u total chunks discovered\n", chunk_count);

  if(chunk_count == 0)
  {
    log_warning("btrfs_remove_used_space: no chunks found, cannot exclude allocated space\n");
    free(sb_buf);
    return 0;
  }

  /* Step 3: Remove allocated chunk ranges from search space.
   * Use the same batching approach as ext2_remove_used_space() for efficiency. */
  for(i = 0; i < chunk_count; i++)
  {
    const uint64_t chunk_start = partition->part_offset + chunks[i].physical;
    const uint64_t chunk_end = chunk_start + chunks[i].length - 1;

    /* Ensure we don't go past partition boundaries */
    if(chunks[i].physical + chunks[i].length > partition->part_size)
    {
      log_warning("btrfs: chunk at physical %llu + %llu exceeds partition size %llu, skipping\n",
          (unsigned long long)chunks[i].physical,
          (unsigned long long)chunks[i].length,
          (unsigned long long)partition->part_size);
      continue;
    }

    /* Batch contiguous ranges for efficiency */
    if(end_free + 1 == chunk_start)
    {
      end_free = chunk_end;
    }
    else
    {
      if(start_free != end_free && start_free != 0)
      {
        del_search_space(list_search_space, start_free, end_free);
        total_excluded += end_free - start_free + 1;
      }
      start_free = chunk_start;
      end_free = chunk_end;
    }
  }

  /* Flush last batch */
  if(start_free != end_free && start_free != 0)
  {
    del_search_space(list_search_space, start_free, end_free);
    total_excluded += end_free - start_free + 1;
  }

  log_info("btrfs_remove_used_space: excluded %llu bytes (%llu MB) of allocated space\n",
      (unsigned long long)total_excluded,
      (unsigned long long)(total_excluded / (1024 * 1024)));

  /* Also exclude the superblock mirror locations */
  {
    const uint64_t sb_mirrors[] = {
      BTRFS_SUPER_INFO_OFFSET,                    /* 64 KiB */
      (uint64_t)64 * 1024 * 1024,                 /* 64 MiB */
      (uint64_t)256 * 1024 * 1024 * 1024          /* 256 GiB */
    };
    unsigned int m;
    for(m = 0; m < 3; m++)
    {
      if(sb_mirrors[m] + BTRFS_SUPER_INFO_SIZE <= partition->part_size)
      {
        del_search_space(list_search_space,
            partition->part_offset + sb_mirrors[m],
            partition->part_offset + sb_mirrors[m] + BTRFS_SUPER_INFO_SIZE - 1);
      }
    }
  }

  free(sb_buf);
  return sectorsize;
}
