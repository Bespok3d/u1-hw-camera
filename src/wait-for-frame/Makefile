TARGET = wait-for-frame.py
SRC = wait-for-frame.py

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

all: $(TARGET)

clean:

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(SRC) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

.PHONY: all clean install uninstall
