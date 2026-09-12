CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=

PREFIX  ?= /usr/local
DESTDIR ?=

BIN     = i8080-emu
SRCS    = main.c cpu.c
OBJS    = main.o cpu.o

.PHONY: all debug clean install uninstall reinstall
 
all: $(BIN)
 
$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $(BIN) $(OBJS) $(LDFLAGS)
 
main.o: main.c cpu.h
	$(CC) $(CFLAGS) -c main.c -o main.o
 
cpu.o: cpu.c cpu.h
	$(CC) $(CFLAGS) -c cpu.c -o cpu.o
 
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
