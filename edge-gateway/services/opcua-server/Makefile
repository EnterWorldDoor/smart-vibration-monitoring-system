#
# OPC UA Server — Makefile
# Mirrors rs232-gateway Makefile patterns (C11, -Wall -Wextra -Werror, .d deps)
#
# open62541 must be installed on the build host (libopen62541-dev or built from source).
# If open62541 is in a non-standard location, override OPEN62541_DIR:
#   make OPEN62541_DIR=/opt/open62541/build
#

CC       := gcc
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O2 -g -D_DEFAULT_SOURCE \
            -I/usr/include/postgresql
LDFLAGS  := -lopen62541 -lpq -lyaml -lpthread -lm

TARGET   := build/opcua-server

SRCDIR   := src
BUILDDIR := build
TESTDIR  := tests

SRCS     := $(SRCDIR)/main.c \
            $(SRCDIR)/opcua_server.c \
            $(SRCDIR)/db_client.c \
            $(SRCDIR)/node_mapping.c \
            $(SRCDIR)/config.c

OBJS     := $(SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
DEPS     := $(OBJS:.o=.d)

# Unit test targets
TEST_OBJS      := $(BUILDDIR)/config.o $(BUILDDIR)/node_mapping.o $(BUILDDIR)/db_client.o $(BUILDDIR)/opcua_server.o
TEST_TARGETS   := $(BUILDDIR)/test_config \
                  $(BUILDDIR)/test_node_mapping \
                  $(BUILDDIR)/test_db_client \
                  $(BUILDDIR)/test_opcua_server

# --- Default target ---
.PHONY: all
all: $(TARGET)

# --- Link final binary ---
$(TARGET): $(OBJS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# --- Compile objects with auto-dependency generation ---
$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# --- Include auto-generated dependency files ---
-include $(DEPS)

# --- Test targets ---
.PHONY: test
test: $(TEST_TARGETS)
	@echo "=== Running unit tests ==="
	@passed=0; failed=0; \
	for t in $(TEST_TARGETS); do \
		echo "--- $$t ---"; \
		if $$t; then passed=$$((passed + 1)); \
		else failed=$$((failed + 1)); fi; \
		echo ""; \
	done; \
	echo "=== $$((passed + failed)) tests run: $$passed passed, $$failed failed ==="; \
	[ $$failed -eq 0 ] || exit 1

$(BUILDDIR)/test_config: $(TESTDIR)/test_config.c $(BUILDDIR)/config.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(SRCDIR) -o $@ $^ -lyaml

$(BUILDDIR)/test_node_mapping: $(TESTDIR)/test_node_mapping.c $(BUILDDIR)/node_mapping.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(SRCDIR) -o $@ $^

$(BUILDDIR)/test_db_client: $(TESTDIR)/test_db_client.c $(BUILDDIR)/db_client.o $(BUILDDIR)/config.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(SRCDIR) -o $@ $^ -lpq -lyaml

$(BUILDDIR)/test_opcua_server: $(TESTDIR)/test_opcua_server.c $(BUILDDIR)/opcua_server.o $(BUILDDIR)/node_mapping.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(SRCDIR) -o $@ $^ $(LDFLAGS)

# --- Install ---
.PHONY: install
install: $(TARGET)
	install -d $(DESTDIR)/usr/local/bin
	install -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/opcua-server
	install -d $(DESTDIR)/etc/systemd/system
	install -m 644 systemd/opcua-server.service $(DESTDIR)/etc/systemd/system/opcua-server.service

# --- Clean ---
.PHONY: clean
clean:
	rm -rf $(BUILDDIR)

# --- Help ---
.PHONY: help
help:
	@echo "Targets:"
	@echo "  all      — Build opcua-server binary (default)"
	@echo "  test     — Run all unit tests"
	@echo "  install  — Install binary + systemd unit"
	@echo "  clean    — Remove build directory"
	@echo ""
	@echo "Variables:"
	@echo "  DESTDIR  — Installation root (default: /)"
