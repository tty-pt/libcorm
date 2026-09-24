/* Single-axis "stub" plugin for roster/load tests (2B-1).
 * Registers ONE axis named "stub" — exercises the by-name dlopen
 * path (`@stub` → `libstub.so`). Rec_axis_open returns a non-NULL
 * dummy ctx so bind() is observable via --list-axes. */
#include <ttypt/rec.h>
#include <stdio.h>
#include <stdlib.h>

static int stub_slot = -1;
static char stub_ctx = 1;

static int
stub_fill(void *ctx, void *params, rec_set_t *out)
{
	(void) ctx; (void) params;
	rec_set_seal(out);
	return 0;
}

__attribute__((constructor))
static void stub_init(void)
{
	static const rec_axis_t a = {
		"stub", stub_fill, NULL, &stub_ctx, NULL
	};
	stub_slot = rec_axis_register(&a);
	(void) stub_slot;
}

void *
rec_axis_open(const char *spec)
{
	(void) spec;
	return &stub_ctx;
}
