/* Dependency cache implementation for GNU make.
Copyright (C) 2014 Free Software Foundation, Inc.
This file is part of GNU Make.

GNU Make is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 3 of the License, or (at your option) any later
version.

GNU Make is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <stdio.h>
#include "depcache.h"
#include "hash.h"

struct cache_target
{
  char *name;
  unsigned int id;
};

struct dep_pair
{
  unsigned int target_id;
  unsigned int dep_id;
};

static unsigned long
by_name_hash (const void *key)
{
  const struct cache_target *target = (const struct cache_target *) key;
  return ((unsigned long) target->name);
}

static int
by_name_cmp (const void *k, const void *s)
{
  const char *key = (char *) k;
  const struct cache_target *slot = ((struct cache_target *) s);
  if (key < slot->name)
    return -1;
  if (key > slot->name)
    return 1;
  return 0;
}

#define DECL_VECTOR(NAME, INITSIZE) \
static struct NAME *first_ ## NAME; \
static struct NAME *end_ ## NAME; \
static struct NAME *reserved_ ## NAME; \
static const unsigned int initial_ ## NAME ## _size = INITSIZE

#define INIT_VECTOR(NAME) \
first_ ## NAME = xmalloc((sizeof (struct NAME)) * initial_ ## NAME ## _size); \
end_ ## NAME = first_ ## NAME; \
reserved_ ## NAME = first_ ## NAME + initial_ ## NAME ## _size \

#define RESERVE_ONE_MORE(NAME) \
if(end_ ## NAME == reserved_ ## NAME) \
{ \
    unsigned int size = end_ ## NAME - first_ ## NAME; \
    first_ ## NAME = (struct NAME*)xrealloc(first_ ## NAME, (sizeof (struct NAME) * 2 * size)); \
    end_ ## NAME = first_ ## NAME + size; \
    reserved_ ## NAME = first_ ## NAME + 2 * size; \
}

static struct hash_table writecache;
static unsigned int writecache_count;
static int recording_dep = 0;
DECL_VECTOR (cache_target, 10000);
DECL_VECTOR (dep_pair, 10000);

void
start_depcache ()
{
  if (recording_dep)
    puts ("warning: inner includedepcache ignored");
  else
    {
      hash_init (&writecache, 10000, by_name_hash, by_name_hash, by_name_cmp);
      writecache_count = 0;
      INIT_VECTOR (cache_target);
      INIT_VECTOR (dep_pair);
    }
  ++recording_dep;
}

static inline unsigned int
writecache_get_id (const char *name)
{
  struct cache_target **slot =
    (struct cache_target **) hash_find_slot (&writecache, name);
  struct cache_target *entry = *slot;
  if (HASH_VACANT (entry))
    {
      struct cache_target *target;
      RESERVE_ONE_MORE (cache_target);
      target = end_cache_target++;
      target->name = (char *) name;
      target->id = writecache_count++;
      hash_insert_at (&writecache, target, slot);
      return target->id;
    }
  else
    return entry->id;
}

void
add_depcache (struct file *f, struct dep *deps)
{
  unsigned int target_id;
  unsigned int dep_id;
  if (!recording_dep)
    return;
  target_id = writecache_get_id (f->name);
  while (deps)
    {
      dep_id = writecache_get_id (deps->file->name);
      RESERVE_ONE_MORE (dep_pair);
      end_dep_pair->target_id = target_id;
      end_dep_pair->dep_id = dep_id;
      ++end_dep_pair;
      deps = deps->next;
    }
}

static char *
get_cachefilename (const char *cachedfilename)
{
  char *fnbuf;
  size_t fnbuflen;
  fnbuflen = strlen (cachedfilename) + 7;
  fnbuf = xmalloc (fnbuflen);
  snprintf (fnbuf, fnbuflen, "%s.cache", cachedfilename);
  return fnbuf;
}

static char **target_order;

static void
order_entry (const void *item)
{
  struct cache_target *target = (struct cache_target *) item;
  *(target_order + target->id) = target->name;
}

void
end_depcache (const char *cachedfilename)
{
  char *cachefilename;
  FILE *cachefile;
  unsigned int i;
  --recording_dep;
  if (recording_dep)
    return;
  cachefilename = get_cachefilename (cachedfilename);
  cachefile = fopen (cachefilename, "w");
  free (cachefilename);
  fprintf (cachefile, "%d\n", writecache_count);
  // reorder the ids as we write them
  target_order = xmalloc (sizeof (char *) * writecache_count);
  hash_map (&writecache, order_entry);
  for (i = 0; i < writecache_count; ++i)
    {
      fputs (*(target_order + i), cachefile);
      fputc ('\n', cachefile);
    }
  fprintf (cachefile, "%u\n", (unsigned int) (end_dep_pair - first_dep_pair));
  fwrite (first_dep_pair, sizeof (struct dep_pair),
	  end_dep_pair - first_dep_pair, cachefile);
  fclose (cachefile);
  hash_free (&writecache, 0);
  free (first_dep_pair);
  free (target_order);
}

#define MAXFILELEN 10240

static unsigned int
read_filenames (FILE * cachefile, struct file ***files)
{
  char *buf;
  struct file **cur_file;
  unsigned int count;
  struct file **files_end;
  buf = xmalloc (MAXFILELEN);
  // read names
  if (!fscanf (cachefile, "%d\n", &count) || feof (cachefile)
      || ferror (cachefile))
    fatal (NILF, "corrupt cache file\n");
  if (count)
    {
      *files = xmalloc (sizeof (struct file *) * count);
      files_end = *files + count;
      for (cur_file = *files; cur_file < files_end; ++cur_file)
	{
	  const char *name;
	  if (!fgets (buf, MAXFILELEN, cachefile) || feof (cachefile)
	      || ferror (cachefile))
	    fatal (NILF, "corrupt cache file\n");
	  buf[strlen (buf) - 1] = 0;
	  name = strcache_add (buf);
	  *cur_file = lookup_file (name);
	  if (!*cur_file)
	    *cur_file = enter_file (name);
	}
    }
  free (buf);
  return count;
}

static void
read_deps (FILE * cachefile, struct file **files,
	   const unsigned int filecount)
{
  struct dep *last_dep;
  struct file *f;
  struct dep_pair *cur_dep_pair;
  unsigned int count;
  unsigned int last_target_id;
  if (!fscanf (cachefile, "%d\n", &count) || feof (cachefile)
      || ferror (cachefile))
    fatal (NILF, "corrupt cache file: no deps count\n");
  first_dep_pair = xmalloc (sizeof (struct dep_pair) * count);
  end_dep_pair = first_dep_pair + count;
  if (fread (first_dep_pair, sizeof (struct dep_pair), count, cachefile) !=
      count || ferror (cachefile))
    fatal (NILF, "corrupt cache file: deps count wrong\n");
  last_target_id = ((unsigned int) -1);
  last_dep = NULL;
  f = NULL;
  for (cur_dep_pair = first_dep_pair; cur_dep_pair < end_dep_pair;
       ++cur_dep_pair)
    {
      struct dep *new_dep;
      if (cur_dep_pair->target_id != last_target_id)
	{
	  last_target_id = cur_dep_pair->target_id;
	  if (last_target_id > filecount)
	    fatal (NILF, "corrupt cache file: index out of bounds\n");
	  f = files[last_target_id];
	  last_dep = f->deps;
	  if (last_dep)
	    while (last_dep->next != 0)
	      last_dep = last_dep->next;
	}
      if (cur_dep_pair->dep_id > filecount)
	    fatal (NILF, "corrupt cache file: index out of bounds\n");
      new_dep = alloc_dep ();
      new_dep->file = files[cur_dep_pair->dep_id];
      new_dep->name = new_dep->file->name;
      if (f->deps)
	{
	  last_dep->next = new_dep;
	}
      else
	{
	  f->deps = new_dep;
	}
      last_dep = new_dep;
    }
  free (first_dep_pair);
}

int
read_depcache (const char *cachedfilename)
{
  FILE *cachefile;
  FILE_TIMESTAMP cachedfiletime;
  FILE_TIMESTAMP cachefiletime;
  char *cachefilename;
  int e;
  struct file **files;
  struct stat st_cached;
  struct stat st_cache;
  unsigned int filecount;
  EINTRLOOP (e, stat (cachedfilename, &st_cached));
  if (e)
    return 0;
  cachedfiletime = FILE_TIMESTAMP_STAT_MODTIME (cachedfilename, st_cached);
  cachefilename = get_cachefilename (cachedfilename);
  EINTRLOOP (e, stat (cachefilename, &st_cache));
  if (e)
    return 0;
  cachefiletime = FILE_TIMESTAMP_STAT_MODTIME (cachefilename, st_cache);
  cachefile = fopen (cachefilename, "r");
  free (cachefilename);
  if ((cachedfiletime > cachefiletime) || !cachefile)
    return 0;
  files = NULL;
  filecount = read_filenames (cachefile, &files);
  if (filecount)
    read_deps (cachefile, files, filecount);
  fclose (cachefile);
  free (files);
  return 1;
}
