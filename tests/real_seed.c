/* tests/real_seed.c — 2B-2 real-file gate seeder (mm-plan/PHASE-2-CLI.md
 * 2B-2): seeds the SAME refs (1/2/3) into the real axis stores living
 * alongside the primary: joint (leading-date), islet (point lists), sepal
 * (floats-direct by default). stoma is deliberately NOT seeded — the 2B-2
 * contract rebuilds it from the primary at each open, never from files.
 *
 * Each axis lib is dlopen'd separately and its rec_axis_open/store
 * dlsym'd from its own handle — the four libs all export the same
 * conventional symbols, so linking them into one binary would collapse
 * them to one provider (exactly the discipline the corm CLI's by-name
 * dlopen enforces). joint_init is likewise dlsym'd, only for the
 * handle-0 burn.
 *
 * Sepal has two columns, chosen by env (D8): floats-direct (default; the
 * gate's offline case) or embedded strings when BOTH CORM_SEPAL_EMBED_URL
 * and CORM_SEPAL_EMBED_MODEL are set (the env-gated second case — the
 * seeder configures the embedder from the same env vars and embeds the
 * query text itself so both columns share one qvec consumer). Either way
 * it writes the binary query-vector file `<dir>/q.vec` plus `<dir>/q.dim`
 * (the sepal leaf reads a qvec file), so the test script builds its `-X`
 * leaf uniformly as `file=q.vec qdim=$(cat q.dim) m=… min_sim=…`.
 *
*  Usage: real_seed <axis-dir1:axis-dir2:...> <primary-path>
 *
 *  Primary-path is the CLI's primary FILE (dir + basename, e.g.
 *  "$td/greps.db"); q.vec/q.dim are written beside it. Axis specs are
 *  per-primary (7-AXIS-NAMESPACE-PLAN.md): <dir>/<basename>-<axis>, so
 *  seeding matches what the CLI binds for THIS primary exactly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <libgen.h>
#include <dlfcn.h>

typedef uint32_t rec_ref_t;
typedef void *(*open_fn)(const char *);
typedef int (*store_fn)(void *, const char *, rec_ref_t, const char *);

struct axis {
	const char *name;
	void *handle;
	open_fn open;
	store_fn store;
	void *ctx;
};

static void axis_load(struct axis *a, const char *pathdirs)
{
	char *copy, *save = NULL, *dir;
	char path[4096];

	a->handle = NULL;
	copy = strdup(pathdirs);
	if (!copy) {
		fprintf(stderr, "real_seed: out of memory\n");
		exit(1);
	}
	for (dir = strtok_r(copy, ":", &save); dir;
	     dir = strtok_r(NULL, ":", &save)) {
		snprintf(path, sizeof(path), "%s/lib%s.so", dir, a->name);
		a->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		if (a->handle)
			break;
	}
	free(copy);
	if (!a->handle) {
		fprintf(stderr, "real_seed: cannot dlopen lib%s.so (%s)\n",
		        a->name, dlerror());
		exit(1);
	}
	a->open = (open_fn)dlsym(a->handle, "rec_axis_open");
	a->store = (store_fn)dlsym(a->handle, "rec_axis_store");
	if (!a->open || !a->store) {
		fprintf(stderr, "real_seed: lib%s.so lacks open/store\n",
		        a->name);
		exit(1);
	}
}

static void axis_open(struct axis *a, const char *spec)
{
	a->ctx = a->open(spec);
	if (!a->ctx) {
		fprintf(stderr, "real_seed: %s open('%s') failed\n",
		        a->name, spec);
		exit(1);
	}
}

static void axis_store(struct axis *a, rec_ref_t ref, const char *value)
{
	if (a->store(a->ctx, NULL, ref, value) != 0) {
		fprintf(stderr, "real_seed: store %s ref %u failed\n",
		        a->name, ref);
		exit(1);
	}
}

static void write_qvec(const char *path, const float *v, size_t n)
{
	FILE *f = fopen(path, "wb");

	if (!f) {
		perror(path);
		exit(1);
	}
	if (fwrite(v, sizeof(float), n, f) != n) {
		fprintf(stderr, "real_seed: short write %s\n", path);
		exit(1);
	}
	fclose(f);
}

static void write_dim(const char *path, size_t n)
{
	FILE *f = fopen(path, "w");

	if (!f) {
		perror(path);
		exit(1);
	}
	fprintf(f, "%zu\n", n);
	fclose(f);
}

int main(int argc, char **argv)
{
	const char *pathdirs, *primary;
	const char *url, *model, *key;
	char dir[4096], base[4096], head[4096];
	int embed;
	char spec[4096], qvec[4096], qdim[4096];
	struct axis joint = { "joint" };
	struct axis islet = { "islet" };
	struct axis sepal = { "sepal" };
	size_t i;
	static const rec_ref_t ref[3] = { 1, 2, 3 };
	static const char *const date[3] = {
		"2026-09-13T20:00:00",   /* ref 1: before the query day */
		"2026-09-15T08:00:00",   /* ref 2: after  the query day */
		"2026-09-14T12:00:00",   /* ref 3: on the query day (winner) */
	};
	static const char *const pts[3] = {
		"4,4",   /* ref 1: spatial distractor */
		"1,1",   /* ref 2: other cell */
		"9,1",   /* ref 3: the queried point (winner) */
	};
	static const char *const vec[3] = {
		"0.5,-0.25,0.75",   /* ref 1 */
		"-1.0,-2.0,-1.0",   /* ref 2: near-opposite of ref 3 */
		"0.9,0.1,0.8",      /* ref 3: the queried vector (winner) */
	};
	/* Embed-mode sepal strings: ref 3 == the query text, refs 1/2 far. */
	static const char *const txt[3] = {
		"alpha distant space",
		"omega farther point",
		"beacon beacon harbor lights",
	};
	static const char *const qtext = "beacon beacon harbor lights";

	if (argc != 3) {
		fprintf(stderr,
		        "usage: real_seed <axis-dirs> <primary-path>\n");
		return 2;
	}
	pathdirs = argv[1];
	primary = argv[2];

	/* Derive <dir> and <basename> from the primary path so axis specs
	 * match what the CLI binds for exactly this primary. */
	{
		char dpath[4096], bpath[4096], *sl;

		snprintf(dpath, sizeof(dpath), "%s", primary);
		snprintf(bpath, sizeof(bpath), "%s", primary);
		sl = strrchr(bpath, '/');
		snprintf(base, sizeof(base), "%s", sl ? sl + 1 : bpath);
		snprintf(dir, sizeof(dir), "%s", dirname(dpath));
	}

	url = getenv("CORM_SEPAL_EMBED_URL");
	model = getenv("CORM_SEPAL_EMBED_MODEL");
	key = getenv("CORM_SEPAL_EMBED_KEY");
	embed = (url && *url && model && *model) ? 1 : 0;

	axis_load(&joint, pathdirs);
	axis_load(&islet, pathdirs);
	axis_load(&sepal, pathdirs);

	if (embed) {
		int (*cfg)(const char *, const char *, const char *);
		int (*fetch)(const char *, float **, size_t *);

		cfg = (int (*)(const char *, const char *, const char *))
			dlsym(sepal.handle, "sepal_configure_embeddings");
		fetch = (int (*)(const char *, float **, size_t *))
			dlsym(sepal.handle, "sepal_embed_fetch");
		if (!cfg || !fetch) {
			fprintf(stderr,
			        "real_seed: sepal lacks embed exports\n");
			return 1;
		}
		if (cfg(url, model, key) != 0) {
			fprintf(stderr,
			        "real_seed: inconsistent embed pair\n");
			return 1;
		}
	}

	/* Burn joint handle 0 (the documented jd-0 ↔ NULL collision, cf.
	 * libjoint joint_axis_store_test.c) so every file-backed open — this
	 * seeder's AND the CLI's later binds — gets a handle >= 1. */
	{
		uint32_t (*init)(char *);

		init = (uint32_t (*)(char *))
			dlsym(joint.handle, "joint_init");
		if (!init) {
			fprintf(stderr,
			        "real_seed: joint lacks joint_init\n");
			return 1;
		}
		(void)init(NULL);
	}

	snprintf(spec, sizeof(spec), "%s/%s-joint", dir, base);
	axis_open(&joint, spec);
	snprintf(spec, sizeof(spec), "%s/%s-islet", dir, base);
	axis_open(&islet, spec);
	snprintf(spec, sizeof(spec), "%s/%s-sepal", dir, base);
	axis_open(&sepal, spec);

	snprintf(qvec, sizeof(qvec), "%s/q.vec", dir);
	snprintf(qdim, sizeof(qdim), "%s/q.dim", dir);

	for (i = 0; i < 3; i++) {
		axis_store(&joint, ref[i], date[i]);
		axis_store(&islet, ref[i], pts[i]);
		axis_store(&sepal, ref[i], embed ? txt[i] : vec[i]);
	}

	/* Query vector file (binary floats, sepal leaf reads it via fread). */
	if (embed) {
		int (*fetch)(const char *, float **, size_t *);
		float *q = NULL;
		size_t n = 0;

		fetch = (int (*)(const char *, float **, size_t *))
			dlsym(sepal.handle, "sepal_embed_fetch");
		if (fetch(qtext, &q, &n) != 0 || !q) {
			fprintf(stderr,
			        "real_seed: embed of query text failed\n");
			return 1;
		}
		write_qvec(qvec, q, n);
		write_dim(qdim, n);
		free(q);
	} else {
		static const float qv[3] = { 0.9f, 0.1f, 0.8f };

		write_qvec(qvec, qv, 3);
		write_dim(qdim, 3);
	}
	return 0;
}