#ifndef HSTEX_FILEDB_H
#define HSTEX_FILEDB_H

#include <stddef.h>

/* The filename databases an installation keeps beside its trees, read in
   and asked directly rather than through a child process. See
   docs/DECISIONS.md, finding-a-file. */
struct hstex_file_db;

/* The database this process shares, built the first time it is asked for
   and unchanged after that. NULL where there is none to build, or where
   the environment names a search this cannot answer for; the caller falls
   back on the tool in either case. */
const struct hstex_file_db *hstex_file_db_shared(void);

/* Where the database says a bare name is, or NULL where it says nothing,
   says more than one thing, or names something that is not a readable
   file now. The answer belongs to the database and outlives the caller. */
const char *hstex_file_db_lookup(const struct hstex_file_db *database,
                                 const char *name);

#endif
