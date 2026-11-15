CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip

TARGET = libmipi-frame-tap.so

# The wire header is shared with the capture, so both ends are built from the one copy.
CFLAGS = -Wall -Wextra -O2 -fPIC -MMD -I../v4l2-mpp/common/capture-common
LDFLAGS = -shared -lpthread -ldl

SRCS = main.c
OBJS = $(SRCS:.c=.o)
DEPS = $(SRCS:.c=.d)

PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib

all: $(TARGET)

-include $(DEPS)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

strip: $(TARGET)
	$(STRIP) $(TARGET)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(LIBDIR)/

.PHONY: all clean strip install
