CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -std=c11 -Iinclude
LDFLAGS ?=
LDLIBS ?=

TARGET_DIR := build

ifeq ($(OS),Windows_NT)
EXE := .exe
PREFIX ?= $(USERPROFILE)/bin
else
EXE :=
PREFIX ?= $(HOME)/.local/bin
endif

BIN := $(TARGET_DIR)/forge$(EXE)
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=$(TARGET_DIR)/%.o)

.PHONY: all clean test install uninstall

all: $(BIN)

$(TARGET_DIR):
	mkdir -p $(TARGET_DIR)

$(TARGET_DIR)/%.o: src/%.c | $(TARGET_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

# Installs the forge binary to a user-level bin directory (no sudo required).
install: $(BIN)
	mkdir -p $(PREFIX)
	cp $(BIN) $(PREFIX)/forge$(EXE)
	@echo "Installed forge to $(PREFIX)/forge$(EXE)"
	@echo "Add $(PREFIX) to your PATH to call 'forge' from anywhere."

uninstall:
	rm -f $(PREFIX)/forge$(EXE)

clean:
	rm -rf $(TARGET_DIR)

test: all
	$(BIN) --help