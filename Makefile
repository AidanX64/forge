CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -std=c11 -Iinclude
LDFLAGS ?=
LDLIBS ?=

TARGET_DIR := build

ifeq ($(OS),Windows_NT)
EXE := .exe
PREFIX ?= $(USERPROFILE)/bin
define make_directory
cmd /C if not exist "$(1)" mkdir "$(1)"
endef
define remove_directory
cmd /C if exist "$(1)" rmdir /S /Q "$(1)"
endef
define install_binary
cmd /C copy /Y "$(1)" "$(2)" >NUL
endef
define remove_file
cmd /C if exist "$(1)" del /Q "$(1)"
endef
else
EXE :=
PREFIX ?= $(HOME)/.local/bin
define make_directory
mkdir -p "$(1)"
endef
define remove_directory
rm -rf "$(1)"
endef
define install_binary
cp "$(1)" "$(2)"
endef
define remove_file
rm -f "$(1)"
endef
endif

BIN := $(TARGET_DIR)/forge$(EXE)
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=$(TARGET_DIR)/%.o)

.PHONY: all clean test install uninstall

all: $(BIN)

$(TARGET_DIR):
	$(call make_directory,$(TARGET_DIR))

$(TARGET_DIR)/%.o: src/%.c | $(TARGET_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

# Installs the forge binary to a user-level bin directory (no sudo required).
install: $(BIN)
	$(call make_directory,$(PREFIX))
	$(call install_binary,$(BIN),$(PREFIX)/forge$(EXE))
	@echo "Installed forge to $(PREFIX)/forge$(EXE)"
	@echo "Add $(PREFIX) to your PATH to call 'forge' from anywhere."

uninstall:
	$(call remove_file,$(PREFIX)/forge$(EXE))

clean:
	$(call remove_directory,$(TARGET_DIR))

test: all
	$(BIN) --help
