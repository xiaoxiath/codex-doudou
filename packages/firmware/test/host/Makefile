# Host unit tests for the firmware's protocol_parse module.
#
#   make test       # build + run
#   make            # just build
#   make clean      # remove artifacts + vendor cache
#
# Requires: c99-capable cc + curl on $PATH. First run downloads cJSON
# into vendor/ (gitignored).

CC      ?= cc
CFLAGS  := -std=c99 -Wall -Wextra -Wno-unused-parameter -O0 -g \
           -I../../main -Ivendor/cjson
LDFLAGS :=

CJSON_VER  := v1.7.18
CJSON_BASE := https://raw.githubusercontent.com/DaveGamble/cJSON/$(CJSON_VER)

OBJS := build/test_protocol.o \
        build/protocol_parse.o \
        build/cJSON.o

BIN  := build/test_runner

.PHONY: all test clean

all: $(BIN)

test: $(BIN)
	@./$(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

build:
	@mkdir -p build

build/test_protocol.o: test_protocol.c vendor/cjson/cJSON.h ../../main/protocol_parse.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/protocol_parse.o: ../../main/protocol_parse.c ../../main/protocol_parse.h vendor/cjson/cJSON.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/cJSON.o: vendor/cjson/cJSON.c vendor/cjson/cJSON.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

vendor/cjson:
	@mkdir -p $@

vendor/cjson/cJSON.c: | vendor/cjson
	curl -fsSL $(CJSON_BASE)/cJSON.c -o $@

vendor/cjson/cJSON.h: | vendor/cjson
	curl -fsSL $(CJSON_BASE)/cJSON.h -o $@

clean:
	rm -rf build vendor
