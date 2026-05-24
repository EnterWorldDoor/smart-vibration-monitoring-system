# RS232 Gateway — Build System
# Platform: aarch64, Ubuntu 22.04, gcc 11.4.0

CC       := gcc
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O2 -g -D_DEFAULT_SOURCE
LDFLAGS  := -lmosquitto -lyaml -lpthread -lm

SRCDIR   := src
BUILDDIR := build
TARGET   := $(BUILDDIR)/rs232-gateway
TESTDIR  := tests

SRCS     := $(SRCDIR)/main.c \
            $(SRCDIR)/serial.c \
            $(SRCDIR)/protocol.c \
            $(SRCDIR)/mqtt_pub.c \
            $(SRCDIR)/proto_to_json.c

OBJS     := $(SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
DEPS     := $(OBJS:.o=.d)

# ---- Unit test sources ----
TEST_SRCS := $(TESTDIR)/test_protocol.c \
             $(TESTDIR)/test_proto_to_json.c \
             $(TESTDIR)/test_serial_mock.c

# Each test binary linked against the object files it needs
# test_protocol needs: protocol.o
# test_proto_to_json needs: proto_to_json.o protocol.o
# test_serial_mock needs: serial.o
TEST_BINS := $(TEST_SRCS:$(TESTDIR)/%.c=$(BUILDDIR)/%)

.PHONY: all clean test install help

all: $(TARGET)

$(TARGET): $(OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# ---- Unit tests ----

$(BUILDDIR)/test_protocol: $(TESTDIR)/test_protocol.c $(BUILDDIR)/protocol.o | $(BUILDDIR)
	$(CC) $(CFLAGS) -Isrc -I$(TESTDIR) -o $@ $< $(BUILDDIR)/protocol.o $(LDFLAGS)

$(BUILDDIR)/test_proto_to_json: $(TESTDIR)/test_proto_to_json.c $(BUILDDIR)/proto_to_json.o $(BUILDDIR)/protocol.o | $(BUILDDIR)
	$(CC) $(CFLAGS) -Isrc -I$(TESTDIR) -o $@ $< $(BUILDDIR)/proto_to_json.o $(BUILDDIR)/protocol.o $(LDFLAGS)

$(BUILDDIR)/test_serial_mock: $(TESTDIR)/test_serial_mock.c $(BUILDDIR)/serial.o | $(BUILDDIR)
	$(CC) $(CFLAGS) -Isrc -I$(TESTDIR) -o $@ $< $(BUILDDIR)/serial.o $(LDFLAGS)

test: $(TEST_BINS)
	@echo "=== Running unit tests ==="
	@failed=0; \
	for t in $(TEST_BINS); do \
		echo "--- $$t ---"; \
		./$$t || failed=1; \
	done; \
	if [ $$failed -eq 0 ]; then \
		echo "=== All tests passed ==="; \
	else \
		echo "=== Some tests FAILED ==="; \
		exit 1; \
	fi

clean:
	rm -rf $(BUILDDIR)

install: $(TARGET)
	install -d $(DESTDIR)/usr/local/bin
	install -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/rs232-gateway
	install -d $(DESTDIR)/etc/systemd/system
	install -m 644 systemd/rs232-gateway.service $(DESTDIR)/etc/systemd/system/
	@echo "Installed. To enable and start:"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable rs232-gateway"
	@echo "  sudo systemctl start rs232-gateway"

help:
	@echo "make          — Build rs232-gateway binary"
	@echo "make test     — Build and run unit tests"
	@echo "make clean    — Remove build artifacts"
	@echo "make install  — Install binary + systemd unit"

-include $(DEPS)
