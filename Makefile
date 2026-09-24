INSTALL_BIN := corm
all := libcorm corm test test_extended test_multivalue test_record rec_test rec_axis_test rec_axis_store_test rec_cli_test rec_axis_bench bench_multivalue bench_rec

LDLIBS-libcorm := -lxxhash -lqsys
LDLIBS-libcorm-Windows := -lmman
install-dep-dlls-Windows := libmman.dll
LDLIBS-bench_multivalue := -lcorm
LDLIBS-bench_rec := -lcorm
LDLIBS-rec_axis_bench := -lcorm
LDLIBS-test := -lcorm
LDLIBS-test_extended := -lcorm
LDLIBS-test_multivalue := -lcorm
LDLIBS-test_record := -lcorm
LDLIBS-rec_test := -lcorm
LDLIBS-rec_axis_test := -lcorm
LDLIBS-rec_axis_store_test := -lcorm -lqsys
LDLIBS-rec_cli_test := -lcorm
LDLIBS-corm := -lcorm -lqsys
LDLIBS-save_test := -lcorm
LDLIBS-librec_axis_mock := -lcorm
LDLIBS-librec_axis_fold := -lcorm

libcorm-obj-y := src/idm.o src/rec.o src/rec_axis.o src/rec_cli.o

CFLAGS += -g

include ../mk/include.mk

# Mock rec_axis plugin (PLAN-REC-QUERY.md §4.4, Part 3.4), test-only: built
# via an explicit standalone rule rather than the ${all}-driven LIB list
# above, since ../mk/include.mk's LIB-obj-y aggregation ($(LIB:%=%-obj-y)
# followed by a $($(...)) double-dereference) only works correctly for a
# single "lib*" target per Makefile — a second one silently drops
# libcorm's own extra objects (idm.o/rec.o/rec_axis.o) off the link line.
# Never linked into corm/libcorm; dlopen'd only by test-cli.sh.
lib/librec_axis_mock.${SO}: src/librec_axis_mock.c lib/libcorm.${SO} lib
	${cc} ${CFLAGS} ${CFLAGS-LIB} -shared -o $@ src/librec_axis_mock.c ${LDFLAGS} ${LDLIBS-librec_axis_mock}

# Single-axis stub plugins (2B-1 roster/load tests): one axis each, the
# plugin file named lib<axis>.so ("stub", "zed") so by-name dlopen
# (`@stub` → $CORM_AXIS_PATH/libstub.so) resolves them. Same standalone-rule
# pattern as the mock (kept out of the LIB list — see note above).
lib/libstub.${SO}: src/librec_axis_stub.c lib/libcorm.${SO} lib
	${cc} ${CFLAGS} ${CFLAGS-LIB} -shared -o $@ src/librec_axis_stub.c ${LDFLAGS} ${LDLIBS-librec_axis_mock}

lib/libzed.${SO}: src/librec_axis_zed.c lib/libcorm.${SO} lib
	${cc} ${CFLAGS} ${CFLAGS-LIB} -shared -o $@ src/librec_axis_zed.c ${LDFLAGS} ${LDLIBS-librec_axis_mock}

# Functional 2B-3 test axes (alpha/beta/pure — mm-plan/2B-3-IMPLEMENTATION.md
# §5). Same multi-axis-one-so + standalone-rule pattern as the mock.
lib/librec_axis_fold.${SO}: src/librec_axis_fold.c lib/libcorm.${SO} lib
	${cc} ${CFLAGS} ${CFLAGS-LIB} -shared -o $@ src/librec_axis_fold.c ${LDFLAGS} ${LDLIBS-librec_axis_fold}

# Minimal string-only 2B-4 test axis ("plain" — mm-plan/2B-4-
# IMPLEMENTATION.md §5): exports rec_axis_store but NO
# rec_axis_store_typed, so the CLI's typed dispatch must fall back to the
# string symbol on binary primaries. Same standalone-rule pattern.
lib/librec_axis_plain.${SO}: src/librec_axis_plain.c lib/libcorm.${SO} lib
	${cc} ${CFLAGS} ${CFLAGS-LIB} -shared -o $@ src/librec_axis_plain.c ${LDFLAGS} ${LDLIBS-librec_axis_fold}

# L1 short-circuit probe (AXIS-EFF plan / test-shortcircuit.sh): one-axis
# plugin whose decode emits a stderr marker for "boom" and whose fill
# empties on "empty" — the black-box observation point for empty-branch
# skipping. Same standalone-rule pattern as the mock/plain plugins.
lib/librec_axis_probe.${SO}: src/librec_axis_probe.c lib/libcorm.${SO} lib
	${cc} ${CFLAGS} ${CFLAGS-LIB} -shared -o $@ src/librec_axis_probe.c ${LDFLAGS} ${LDLIBS-librec_axis_fold}

all: lib/librec_axis_mock.${SO} lib/libstub.${SO} lib/libzed.${SO} lib/librec_axis_fold.${SO} lib/librec_axis_plain.${SO} lib/librec_axis_probe.${SO}

test: all
	./test.sh
	./test-cli.sh
	./test-roster.sh
	./test-fanout.sh
	./test-real.sh
	./test-mm.sh
	./test-shortcircuit.sh

bench: all
	LD_LIBRARY_PATH=./lib ./bin/bench_multivalue
