/* test_persist.c
 * Multi-database file persistence regression tests.
 *
 * Unlike every other in-tree test binary these need a *second process*:
 * the write phase has to exit (or explicitly save) before the read phase
 * opens the file, because a handle opened in this process is aliased back
 * by corm_open (the 1.4.0 F4 duplicate-open fix) and corm_save() runs
 * from an atexit destructor. Run as:
 *
 *   test_persist w   write phase  (creates the file)
 *   test_persist r   read phase   (asserts every database came back)
 *   test_persist z   close phase  (asserts save-after-close keeps the file)
 *
 * test.sh drives the three in that order.
 */

#include <ttypt/corm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define FILE_DB "p.db"

/* Three databases sharing one file, with the keys named after them so a
 * cross-wired load (a block put into the wrong handle) is visible. */
static const char *dbs[] = { "pa", "pb", "pc" };
#define NDB (sizeof(dbs) / sizeof(dbs[0]))

static const char *kname(const char *db)
{
	static char buf[16];
	snprintf(buf, sizeof(buf), "%s-key", db);
	return buf;
}

static const char *vname(const char *db)
{
	static char buf[16];
	snprintf(buf, sizeof(buf), "%s-val", db);
	return buf;
}

/* Write phase: one key per database, then save. */
static int phase_write(void)
{
	size_t i;

	for (i = 0; i < NDB; i++) {
		uint32_t hd = corm_open(FILE_DB, dbs[i],
		    CM_STR, CM_STR, 0, 0);

		if (hd == CM_MISS) {
			fprintf(stderr, "open %s failed\n", dbs[i]);
			return 1;
		}

		if (corm_put(hd, kname(dbs[i]), vname(dbs[i])) == CM_MISS) {
			fprintf(stderr, "put %s failed\n", dbs[i]);
			return 1;
		}
	}

	corm_save();
	return 0;
}

/* Read phase: reopen every database, deliberately in a different order than
 * the writer used, and require all of them back. Opening in a fresh process
 * is the whole point -- the pre-1.4.1 loader walked the mapping by counting
 * the file's registered handles, so a database whose block was not at the
 * position that walk reached came back empty (and the exit-save then
 * truncated the block it had missed, shrinking the file on disk).
 *
 * corm_save() runs from the destructor here too, so this phase also proves
 * the round trip is stable: a correct load re-writes an identical file. */
static int phase_read(void)
{
	/* Reverse of the write order, so a positional walk cannot pass by
	 * accident the way it did when the reader matched the writer. */
	static const size_t order[] = { 2, 1, 0 };
	size_t i;
	int errors = 0;

	for (i = 0; i < NDB; i++) {
		const char *db = dbs[order[i]];
		const char *got;
		uint32_t hd = corm_open(FILE_DB, db, CM_STR, CM_STR, 0, 0);

		if (hd == CM_MISS) {
			fprintf(stderr, "reopen %s failed\n", db);
			errors++;
			continue;
		}

		got = corm_get(hd, kname(db));

		if (!got || strcmp(got, vname(db))) {
			fprintf(stderr, "%s: %s -> %s (want %s)\n", db,
			    kname(db), got ? got : "(nil)", vname(db));
			errors++;
		}
	}

	if (!errors)
		printf("multi-db load: all %zu databases restored\n", NDB);

	return errors ? 1 : 0;
}

/* Close phase: a file whose only handle was closed has nothing left to
 * save. corm_save_file() used to ftruncate() the path to 0 bytes before
 * noticing, throwing away data it had no intention of rewriting. */
static int phase_close(void)
{
	struct stat sb;
	uint32_t hd = corm_open(FILE_DB, "pz", CM_STR, CM_STR, 0, 0);

	if (hd == CM_MISS) {
		fprintf(stderr, "open pz failed\n");
		return 1;
	}

	if (corm_put(hd, "pz-key", "pz-val") == CM_MISS) {
		fprintf(stderr, "put pz failed\n");
		return 1;
	}

	corm_save();
	corm_close(hd);
	corm_save();

	if (stat(FILE_DB, &sb) == -1) {
		fprintf(stderr, "stat %s failed\n", FILE_DB);
		return 1;
	}

	if (!sb.st_size) {
		fprintf(stderr, "save after close truncated %s to 0 bytes\n",
		    FILE_DB);
		return 1;
	}

	printf("save after close: %s kept (%ld bytes)\n",
	    FILE_DB, (long) sb.st_size);

	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s w|r|z\n", argv[0]);
		return 2;
	}

	if (!strcmp(argv[1], "w"))
		return phase_write();

	if (!strcmp(argv[1], "r"))
		return phase_read();

	if (!strcmp(argv[1], "z"))
		return phase_close();

	fprintf(stderr, "unknown phase %s\n", argv[1]);
	return 2;
}
