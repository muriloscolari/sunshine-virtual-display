CC ?= gcc
CFLAGS ?= -Wall -Wextra -Wpedantic -std=c11 -O2
LDFLAGS ?= -lcjson
TARGET = sunshine-display-daemon
SRC = sunshine-display-daemon.c

PREFIX ?= $(HOME)/.local
BINDIR = $(PREFIX)/bin

.PHONY: all clean install test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

install: $(TARGET)
	mkdir -p $(BINDIR)
	cp -f $(TARGET) $(BINDIR)/$(TARGET)
	chmod 755 $(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)

test:
	$(CC) -Wall -Wextra -Wpedantic -std=c11 -fsanitize=address,undefined -g $(SRC) -o /tmp/daemon_test $(LDFLAGS)
	/tmp/daemon_test --test-iterations 3
	rm -f /tmp/daemon_test
