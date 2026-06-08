CC ?= cc
PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags gtk+-3.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs gtk+-3.0) -lutil

TARGET := gbashrun
SRC := gbashrun.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)
