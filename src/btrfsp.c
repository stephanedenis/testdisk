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

/* On-disk key (packed: 8 + 1 + 8 = 17 bytes) */
struct btrfs_disk_key {
  uint64_t objectid;
  uint8_t  type;
  uint64_t offset;
} __attribute__ ((gcc_struct, __packed__));

/* Chunk item stripe (packed: 8 + 8 + 16 = 32 bytes) */
struct btrfs_stripe {
  uint64_t devid;
  uint64_t offset;      /* physical offset on device */
  uint8_t  dev_uuid[BTRFS_UUID_SIZE];
} __attribute__ ((gcc_struct, __packed__));

/* Chunk item (packed: 48 bytes + first stripe 32 = 80 total) */
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

/* B-tree node header (packed: 101 bytes) */
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

/* Leaf item (in a leaf node, level==0): key(17) + offset(4) + size(4) = 25 bytes */
struct btrfs_item {
  struct btrfs_disk_key key;
  uint32_t offset;  /* offset relative to the end of the header area */
  uint32_t size;    /* size of the data */
} __attribute__ ((gcc_struct, __packed__));

/* Internal node key pointer (in internal nodes, level>0): key(17) + blockptr(8) + gen(8) = 33 bytes */
struct btrfs_key_ptr {
  struct btrfs_disk_key key;
  uint64_t blockptr;    /* logical address of child node */
  uint64_t generation;
} __attribute__ ((gcc_struct, __packed__));

/*
 * Strategy for excluding allocated btrfs space:
 *
 * 1. Read the btrfs superblock to get sectorsize, nodesize, and sys_chunk_array
 * 2. Parse sys_chunk_array to get system chunk logical->physical mappings
 *    (these bootstrap the ability to read the chunk tree)
 * 3. Use logical->physical translation to read the chunk tree root
 * 4. Walk the chunk tree B-tree to discover ALL chunk mappings
 * 5. For each chunk: its stripes tell us the physical byte ranges on disk
 * 6. Call del_search_space() for each physical range that is allocated
 *
 * CRITICAL: btrfs uses separate logical and physical address spaces.
 * The superblock's chunk_root and all B-tree pointers are LOGICAL addresses.
 * They must be translated to physical addresses using the chunk map before
 * we can read them from disk.
 */

/* Maximum number of chunks we track */
#define MAX_CHUNKS 65536

struct chunk_info {
  uint64_t logical;    /* logical offset in btrfs address space */
  uint64_t physical;   /* physical offset on this device */
  uint64_t length;     /* length of the chunk */
  uint64_t type;       /* chunk type flags */
};

static unsigned int chunk_count = 0;
static struct chunk_info chunks[MAX_CHUNKS];

static void add_chunk(const uint64_t logical, const uint64_t physical,
    const uint64_t length, const uint64_t type)
{
  if(chunk_count >= MAX_CHUNKS)
  {
    log_warning("btrfs: too many chunks (>%u), some allocated space may not be excluded\n",
        MAX_CHUNKS);
    return;
  }
  chunks[chunk_count].logical = logical;
  chunks[chunk_count].physical = physical;
  chunks[chunk_count].length = length;
  chunks[chunk_count].type = type;
  chunk_count++;
}

/*
 * Translate a btrfs logical address to a physical device address.
 * Uses the chunk map built from sys_chunk_array and chunk tree.
 * Returns 1 on success (physical_addr is set), 0 if no mapping found.
 */
static int logical_to_physical(const uint64_t logical_addr, uint64_t *physical_addr)
{
  unsigned int i;
  for(i = 0; i < chunk_count; i++)
  {
    if(logical_addr >= chunks[i].logical &&
       logical_addr < chunks[i].logical + chunks[i].length)
    {
      *physical_addr = chunks[i].physical + (logical_addr - chunks[i].logical);
      return 1;
    }
  }
  return 0;
}

/*
 * Parse the sys_chunk_array embedded in the superblock.
 * This gives us the system chunks needed to bootstrap reading the chunk tree.
 * Without these mappings, we cannot translate chunk_root's logical address
 * to a physical disk offset.
 */
static void parse_sys_chunk_array(const struct btrfs_super_block *sb)
{
  const uint8_t *array = sb->sys_chunk_array;
  const uint32_t array_size = le32(sb->sys_chunk_array_size);
  uint32_t offset = 0;

  log_info("btrfs: parsing sys_chunk_array, size=%u bytes\n", array_size);

  while(offset < array_size)
  {
    const struct btrfs_disk_key *key;
    const struct btrfs_chunk *chunk;
    uint16_t num_stripes;
    uint16_t i;
    uint64_t logical;

    if(offset + sizeof(struct btrfs_disk_key) > array_size)
      break;

    key = (const struct btrfs_disk_key *)(array + offset);
    offset += sizeof(struct btrfs_disk_key);

    if(key->type != BTRFS_CHUNK_ITEM_KEY)
    {
      log_warning("btrfs: unexpected key type %u in sys_chunk_array at offset %u\n",
          key->type, offset);
      break;
    }

    if(offset + sizeof(struct btrfs_chunk) > array_size)
      break;

    chunk = (const struct btrfs_chunk *)(array + offset);
    num_stripes = le16(chunk->num_stripes);
    logical = le64(key->offset);

    {
      const char *type_str = "UNKNOWN";
      const uint64_t ctype = le64(chunk->type);
      if(ctype & BTRFS_BLOCK_GROUP_SYSTEM)   type_str = "SYSTEM";
      if(ctype & BTRFS_BLOCK_GROUP_METADATA) type_str = "METADATA";
      if(ctype & BTRFS_BLOCK_GROUP_DATA)     type_str = "DATA";
      log_info("btrfs: sys_chunk: logical=0x%llx length=%llu (%llu MiB) type=%s stripes=%u\n",
          (unsigned long long)logical,
          (unsigned long long)le64(chunk->length),
          (unsigned long long)(le64(chunk->length) / (1024*1024)),
          type_str, num_stripes);
    }

    /* Add each stripe's physical location */
    if(num_stripes > 0)
    {
      log_info("btrfs:   stripe[0]: physical=0x%llx devid=%llu\n",
          (unsigned long long)le64(chunk->stripe.offset),
          (unsigned long long)le64(chunk->stripe.devid));
      add_chunk(logical, le64(chunk->stripe.offset),
          le64(chunk->length), le64(chunk->type));

      /* Additional stripes follow the chunk struct */
      for(i = 1; i < num_stripes; i++)
      {
        const struct btrfs_stripe *extra_stripe;
        const uint32_t stripe_offset = offset + sizeof(struct btrfs_chunk) +
          (uint32_t)(i - 1) * sizeof(struct btrfs_stripe);
        if(stripe_offset + sizeof(struct btrfs_stripe) > array_size)
          break;
        extra_stripe = (const struct btrfs_stripe *)(array + stripe_offset);
        log_info("btrfs:   stripe[%u]: physical=0x%llx devid=%llu\n",
            i,
            (unsigned long long)le64(extra_stripe->offset),
            (unsigned long long)le64(extra_stripe->devid));
        add_chunk(logical, le64(extra_stripe->offset),
            le64(chunk->length), le64(chunk->type));
      }
    }

    /* Advance past the chunk struct and all its stripes */
    offset += sizeof(struct btrfs_chunk) +
      (uint32_t)(num_stripes > 0 ? num_stripes - 1 : 0) * sizeof(struct btrfs_stripe);
  }
}

/*
 * Parse a single leaf node of the chunk tree.
 * Extract all CHUNK_ITEM entries and add their physical stripes to the map.
 */
static void parse_chunk_tree_leaf(
    const unsigned char *leaf_buf, const uint32_t nodesize)
{
  const struct btrfs_header *header = (const struct btrfs_header *)leaf_buf;
  const uint32_t nritems = le32(header->nritems);
  uint32_t i;

  if(header->level != 0)
    return;  /* Not a leaf */

  log_trace("btrfs: parsing chunk tree leaf with %u items\n", nritems);

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
      uint64_t logical;

      if(data_offset + sizeof(struct btrfs_chunk) > nodesize)
        continue;

      chunk = (const struct btrfs_chunk *)(leaf_buf + data_offset);
      num_stripes = le16(chunk->num_stripes);
      logical = le64(item->key.offset);

      log_trace("btrfs: chunk: logical=0x%llx length=%llu type=0x%llx stripes=%u\n",
          (unsigned long long)logical,
          (unsigned long long)le64(chunk->length),
          (unsigned long long)le64(chunk->type),
          num_stripes);

      /* First stripe */
      if(num_stripes > 0)
      {
        add_chunk(logical, le64(chunk->stripe.offset),
            le64(chunk->length), le64(chunk->type));
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
        add_chunk(logical, le64(extra->offset),
            le64(chunk->length), le64(chunk->type));
      }
    }
  }
}

/*
 * Recursively walk the chunk tree from a given node.
 * - If level==0 (leaf): parse chunk items
 * - If level>0 (internal): read child nodes and recurse
 *
 * IMPORTANT: node_bytenr is a LOGICAL address. We must translate it
 * to physical using the chunk map before reading from disk.
 * max_depth prevents runaway recursion on corrupt trees.
 */
static void walk_chunk_tree(disk_t *disk, const partition_t *partition,
    const uint64_t node_bytenr, const uint32_t nodesize, const int max_depth)
{
  unsigned char *buf;
  const struct btrfs_header *header;
  uint64_t physical_addr;

  if(max_depth <= 0)
  {
    log_warning("btrfs: chunk tree walk exceeded max depth\n");
    return;
  }

  /* Translate logical address to physical using chunk map */
  if(!logical_to_physical(node_bytenr, &physical_addr))
  {
    log_warning("btrfs: cannot translate chunk tree node logical addr 0x%llx to physical "
        "(have %u chunk mappings)\n",
        (unsigned long long)node_bytenr, chunk_count);
    return;
  }

  log_trace("btrfs: reading chunk tree node: logical=0x%llx -> physical=0x%llx\n",
      (unsigned long long)node_bytenr, (unsigned long long)physical_addr);

  buf = (unsigned char *)MALLOC(nodesize);
  if(buf == NULL)
    return;

  if(disk->pread(disk, buf, nodesize,
        partition->part_offset + physical_addr) != (int)nodesize)
  {
    log_warning("btrfs: failed to read chunk tree node at physical 0x%llx (logical 0x%llx)\n",
        (unsigned long long)physical_addr,
        (unsigned long long)node_bytenr);
    free(buf);
    return;
  }

  header = (const struct btrfs_header *)buf;

  /* Validate: header->bytenr should be the logical address of this node */
  if(le64(header->bytenr) != node_bytenr)
  {
    log_warning("btrfs: chunk tree node bytenr mismatch: expected logical 0x%llx, "
        "header says 0x%llx\n",
        (unsigned long long)node_bytenr,
        (unsigned long long)le64(header->bytenr));
    /* Continue anyway - might still have valid data */
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

    log_trace("btrfs: chunk tree internal node level=%u items=%u\n",
        header->level, nritems);

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

unsigned int btrfs_remove_used_space(disk_t *disk, const partition_t *partition,
    alloc_data_t *list_search_space)
{
  struct btrfs_super_block *sb;
  unsigned char *sb_buf;
  uint32_t sectorsize;
  uint32_t nodesize;
  uint64_t chunk_root;
  unsigned int i;
  uint64_t total_excluded = 0;
  unsigned int data_chunks = 0;
  unsigned int meta_chunks = 0;
  unsigned int sys_chunks = 0;

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

  log_info("btrfs_remove_used_space: sectorsize=%u nodesize=%u\n", sectorsize, nodesize);
  log_info("btrfs_remove_used_space: chunk_root logical=0x%llx\n",
      (unsigned long long)chunk_root);
  log_info("btrfs_remove_used_space: total_bytes=%llu (%llu GiB)\n",
      (unsigned long long)le64(sb->total_bytes),
      (unsigned long long)(le64(sb->total_bytes) / (1024ULL*1024*1024)));
  log_info("btrfs_remove_used_space: bytes_used=%llu (%llu GiB)\n",
      (unsigned long long)le64(sb->bytes_used),
      (unsigned long long)(le64(sb->bytes_used) / (1024ULL*1024*1024)));

  /* Step 1: Parse sys_chunk_array from superblock for bootstrap chunk mappings.
   * This provides the logical->physical mapping for system chunks,
   * which is required to locate the chunk tree root on disk. */
  parse_sys_chunk_array(sb);

  log_info("btrfs_remove_used_space: %u chunk mappings from sys_chunk_array\n", chunk_count);

  /* Step 2: Walk the chunk tree to discover all chunk mappings.
   * The chunk_root is a LOGICAL address - we use the sys_chunk_array
   * mappings to translate it to a physical address for reading. */
  {
    uint64_t chunk_root_phys;
    if(!logical_to_physical(chunk_root, &chunk_root_phys))
    {
      log_error("btrfs_remove_used_space: cannot translate chunk_root logical 0x%llx "
          "to physical (sys_chunk_array may be incomplete)\n",
          (unsigned long long)chunk_root);
      free(sb_buf);
      return 0;
    }
    log_info("btrfs_remove_used_space: chunk_root physical=0x%llx\n",
        (unsigned long long)chunk_root_phys);
  }

  walk_chunk_tree(disk, partition, chunk_root, nodesize, 8);

  log_info("btrfs_remove_used_space: %u total chunk mappings discovered\n", chunk_count);

  if(chunk_count == 0)
  {
    log_warning("btrfs_remove_used_space: no chunks found, cannot exclude allocated space\n");
    free(sb_buf);
    return 0;
  }

  /* Categorize chunks */
  for(i = 0; i < chunk_count; i++)
  {
    if(chunks[i].type & BTRFS_BLOCK_GROUP_DATA)     data_chunks++;
    if(chunks[i].type & BTRFS_BLOCK_GROUP_METADATA) meta_chunks++;
    if(chunks[i].type & BTRFS_BLOCK_GROUP_SYSTEM)   sys_chunks++;
  }
  log_info("btrfs_remove_used_space: chunk types: %u DATA, %u METADATA, %u SYSTEM\n",
      data_chunks, meta_chunks, sys_chunks);

  /* Step 3: Remove allocated chunk ranges from search space.
   * We exclude ALL chunk types (data, metadata, system) because:
   * - Metadata/system chunks contain no user file data
   * - Data chunks contain currently-allocated file extents
   * The unallocated device space (not in any chunk) is where we scan. */
  for(i = 0; i < chunk_count; i++)
  {
    const uint64_t chunk_start = partition->part_offset + chunks[i].physical;
    const uint64_t chunk_end = chunk_start + chunks[i].length - 1;

    /* Ensure we don't go past partition boundaries */
    if(chunks[i].physical + chunks[i].length > partition->part_size)
    {
      log_warning("btrfs: chunk physical 0x%llx + 0x%llx exceeds partition size 0x%llx, clamping\n",
          (unsigned long long)chunks[i].physical,
          (unsigned long long)chunks[i].length,
          (unsigned long long)partition->part_size);
      continue;
    }

    log_trace("btrfs: excluding physical range 0x%llx - 0x%llx (%llu MiB, type=0x%llx)\n",
        (unsigned long long)chunk_start,
        (unsigned long long)chunk_end,
        (unsigned long long)(chunks[i].length / (1024*1024)),
        (unsigned long long)chunks[i].type);

    del_search_space(list_search_space, chunk_start, chunk_end);
    total_excluded += chunks[i].length;
  }

  log_info("btrfs_remove_used_space: excluded %llu bytes (%llu GiB) of allocated chunk space\n",
      (unsigned long long)total_excluded,
      (unsigned long long)(total_excluded / (1024ULL*1024*1024)));
  log_info("btrfs_remove_used_space: remaining search space ≈ %llu GiB\n",
      (unsigned long long)((partition->part_size - total_excluded) / (1024ULL*1024*1024)));

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
