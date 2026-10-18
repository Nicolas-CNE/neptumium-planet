# Planet - package manager with a CDCL MaxSAT dependency solver.
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -Isrc

SRC_DIR := src
BUILD   := build

# Everything the tests link against: the SAT/MaxSAT core plus the package
# data model, the encoding layer, the repo/network layer, the local database,
# the payload fetch/verify/extract layer and the ALPM hooks layer.
CORE_SRC := $(SRC_DIR)/sat.c $(SRC_DIR)/amo.c $(SRC_DIR)/dimacs.c \
            $(SRC_DIR)/maxsat.c $(SRC_DIR)/pkg.c $(SRC_DIR)/encode.c \
            $(SRC_DIR)/net.c $(SRC_DIR)/archrepo.c \
            $(SRC_DIR)/sha256.c $(SRC_DIR)/pkgfile.c $(SRC_DIR)/localdb.c \
            $(SRC_DIR)/hooks.c
CORE_OBJ := $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(CORE_SRC))

# External libraries for the network/repo layer: libcurl (fetch) and
# libarchive (tar.gz database reading, and later .pkg.tar.zst payloads).
LDLIBS   := -lcurl -larchive

# One binary per test_*.c source.
TESTS := test_solver test_assumps test_engine test_maxsat test_pkg test_encode \
         test_repo test_sha256 test_localdb test_hooks

# The planet CLI itself: main.c plus the shared core objects.
PLANET := $(BUILD)/planet

.PHONY: all planet test clean

all: $(addprefix $(BUILD)/,$(TESTS))

planet: $(PLANET)

$(PLANET): $(SRC_DIR)/main.c $(CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(CORE_OBJ) $(LDLIBS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# test binary = its own source ($<) + the shared core objects.
# The test source is listed FIRST so $< resolves to it in the recipe.
$(BUILD)/test_%: $(SRC_DIR)/test_%.c $(CORE_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(CORE_OBJ) $(LDLIBS)

test: all
	@set -e; for t in $(TESTS); do \
	  $(BUILD)/$$t; echo; \
	done

clean:
	rm -rf $(BUILD)
