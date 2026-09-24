/* Single-axis "zed" plugin for roster/load tests (2B-1).
 * Registers ONE axis named "zed" — exercises the by-name dlopen
 * path (`@zed` → `libzed.so`). Same shape as stub. */
#include <ttypt/rec.h>
#include <stdio.h>
#include <stdlib.h>

static int zed_slot = -1;
static char zed_ctx = 1;

static int
zed_fill(void *ctx, void *params, rec_set_t *out)
{
	(void) ctx; (void) params;
	rec_set_seal(out);
	return 0;
}

__attribute__((constructor))
static void zed_init(void)
{
	static const rec_axis_t a = {
		"zed", zed_fill, NULL, &zed_ctx, NULL
	};
	zed_slot = rec_axis_register(&a);
	(void) zed_slot;
}

void *
rec_axis_open(const char *spec)
{
	(void) spec;
	return &zed_ctx;
}
