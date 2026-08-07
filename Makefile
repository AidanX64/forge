CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -std=c11 -Iinclude
LDFLAGS ?=
LDLIBS ?=

TARGET_DIR := target
BIN := $(TARGET_DIR)/forge
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=$(TARGET_DIR)/%.o)

.PHONY: all clean test

all: $(BIN)

$(TARGET_DIR):
	mkdir -p $(TARGET_DIR)

$(TARGET_DIR)/%.o: src/%.c | $(TARGET_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -rf $(TARGET_DIR)

test: all
	$(BIN) --help
