/* fanout_verify.c — read-side probe for the 2B-4 write-fan-out gate
 * (external/libcorm/test-fanout.sh). dlopen-only (no link deps): the
 * helper opens an axis ctx through the plugin's own `rec_axis_open` and
 * reads back what the CLI fan-out stored, or reads a primary's raw u32.
 *
 *   fanout_verify readback <plugin.so> <alongside-spec> <ref>...
 *     → for each ref: "<ref>:<stored>" (empty when absent), exit 0.
 *       Requires the plugin's rec_axis_open + rec_axis_readback symbols.
 *
 *   fanout_verify pu32 <file> <ref>
 *     → "<u32-decimal>" of the primary's stored HNDL/U32 payload ("MISS"
 *       when absent), exit 0. Mask = CORM_MASK-or-4095, matching the CLI.
 *
 * Modes print to stdout; diagnostics to stderr. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t
eff_mask(void)
{
	const char *e = getenv("CORM_MASK");
	if (e && *e) {
		unsigned long v = strtoul(e, NULL, 10);
		if (v != 0 && (v & (v + 1)) == 0)
			return (uint32_t) v;
	}
	return 4096 - 1;
}

static void *
must_sym(void *h, const char *name)
{
	void *sym = dlsym(h, name);
	if (!sym) {
		fprintf(stderr, "fanout_verify: no symbol '%s'\n", name);
		exit(1);
	}
	return sym;
}

int
main(int argc, char *argv[])
{
	if (argc >= 2 && !strcmp(argv[1], "readback")) {
		void *h, *open_sym, *rb_sym;
		void *(*open_fn)(const char *);
		int (*rb_fn)(void *, uint32_t, char **, size_t *);
		void *ctx;

		if (argc < 5) {
			fprintf(stderr, "usage: %s readback <plugin.so> "
					"<spec> <ref>...\n", argv[0]);
			return 1;
		}
		h = dlopen(argv[2], RTLD_NOW);
		if (!h) {
			fprintf(stderr, "fanout_verify: dlopen %s: %s\n",
					argv[2], dlerror());
			return 1;
		}
		open_sym = must_sym(h, "rec_axis_open");
		rb_sym = must_sym(h, "rec_axis_readback");
		memcpy(&open_fn, &open_sym, sizeof(open_fn));
		memcpy(&rb_fn, &rb_sym, sizeof(rb_fn));
		ctx = open_fn(argv[3]);
		if (!ctx) {
			fprintf(stderr, "fanout_verify: rec_axis_open(%s) "
					"returned NULL\n", argv[3]);
			return 1;
		}
		for (int i = 4; i < argc; i++) {
			uint32_t ref = (uint32_t) strtoul(argv[i], NULL, 10);
			char *blob = NULL;
			size_t n = 0;
			if (rb_fn(ctx, ref, &blob, &n) != 0) {
				fprintf(stderr, "fanout_verify: readback(%u) "
						"failed\n", ref);
				return 1;
			}
			printf("%u:%s\n", ref, blob ? blob : "");
			free(blob);
		}
		return 0;
	}

	if (argc == 4 && !strcmp(argv[1], "pu32")) {
		void *h, *open_sym, *get_sym;
		uint32_t (*open_fn)(const char *, const char *,
				uint32_t, uint32_t, uint32_t, uint32_t);
		const void *(*get_fn)(uint32_t, const void *);
		uint32_t hd, ref;
		const void *v;
		uint32_t u;

		h = dlopen("./lib/libcorm.so", RTLD_NOW);
		if (!h) {
			fprintf(stderr, "fanout_verify: dlopen libcorm: %s\n",
					dlerror());
			return 1;
		}
		open_sym = must_sym(h, "corm_open");
		get_sym = must_sym(h, "corm_get");
		memcpy(&open_fn, &open_sym, sizeof(open_fn));
		memcpy(&get_fn, &get_sym, sizeof(get_fn));
		/* HNDL keys, U32 values, "hd" dbid, CM_AINDEX=1 — the CLI's
		 * gen_open shape for :a:u primaries. */
		hd = open_fn(argv[2], "hd", 1, 3, eff_mask(), 1);
		ref = (uint32_t) strtoul(argv[3], NULL, 10);
		v = get_fn(hd, &ref);
		if (!v) {
			printf("MISS\n");
			return 0;
		}
		memcpy(&u, v, sizeof(u));
		printf("%u\n", u);
		return 0;
	}

	fprintf(stderr, "usage: %s {readback|pu32} ...\n", argv[0]);
	return 1;
}
