CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
LDFLAGS ?=
LDLIBS  ?= -lX11          # Xlib

TARGET  := main
SRC     := main.c

CLANG_FORMAT ?= clang-format

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	$(RM) $(TARGET)

.PHONY: format
format:
	$(CLANG_FORMAT) -i *.c
