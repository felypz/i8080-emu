CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=

PREFIX  ?= /usr/local
DESTDIR ?=

SRCDIR  = src
BIN     = i8080-emu
SRCS    = $(SRCDIR)/main.c $(SRCDIR)/cpu.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all debug clean install uninstall reinstall

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $(BIN) $(OBJS) $(LDFLAGS)

$(SRCDIR)/main.o: $(SRCDIR)/main.c $(SRCDIR)/cpu.h
	$(CC) $(CFLAGS) -c $(SRCDIR)/main.c -o $(SRCDIR)/main.o

$(SRCDIR)/cpu.o: $(SRCDIR)/cpu.c $(SRCDIR)/cpu.h
	$(CC) $(CFLAGS) -c $(SRCDIR)/cpu.c -o $(SRCDIR)/cpu.o

debug: CFLAGS += -g -O0 -DDEBUG
debug: clean $(BIN)

clean:
	rm -f $(OBJS) $(BIN)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

reinstall: uninstall install
