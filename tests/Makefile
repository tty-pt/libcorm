# Standalone, disposable PoC harness -- NOT wired into ../Makefile's
# `all`/`test` targets on purpose (AXIS-ID-UNIFORM quest, step 2). Depends
# on sibling repos outside this repo (libjoint, libsepal); run manually:
#
#   make -C external/libcorm/tests
#
# Links against the system-installed libcorm.so/libqsys.so (the same ones
# libjoint.so/libsepal.so were built against -- one loaded copy of each,
# avoids duplicate global-registry state at runtime) and this repo's
# ttypt/corm.h + ttypt/rec.h (byte-identical to the installed copies).

JOINT_DIR := $(HOME)/libjoint
SEPAL_DIR := $(HOME)/libsepal
CORM_INC  := ../include

CFLAGS  := -Wall -Wextra -Wpedantic -g \
           -I$(CORM_INC) -I$(JOINT_DIR)/include -I$(SEPAL_DIR)/include
LDFLAGS := -L$(JOINT_DIR)/lib -L$(SEPAL_DIR)/lib
LDLIBS  := -ljoint -lsepal -lcorm -lqsys
RUN_LD_PATH := $(JOINT_DIR)/lib:$(SEPAL_DIR)/lib

all: fixture_id_join

fixture_id_join: fixture_id_join.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

test: fixture_id_join
	LD_LIBRARY_PATH=$(RUN_LD_PATH) ./fixture_id_join

clean:
	rm -f fixture_id_join

.PHONY: all test clean
