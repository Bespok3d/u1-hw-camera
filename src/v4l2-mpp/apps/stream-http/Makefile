TARGET = camera-stream.py
SRC = camera-stream.py
HTML_FILES = html/index.html html/player.html html/webrtc.html html/control.html

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
HTMLDIR = /usr/share/camera-stream/html

all:
	@echo "Nothing to build for $(TARGET)"

clean:
	@echo "Nothing to clean for $(TARGET)"

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -d $(DESTDIR)$(HTMLDIR)
	install -m 644 $(HTML_FILES) $(DESTDIR)$(HTMLDIR)/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -rf $(DESTDIR)/usr/share/camera-stream

.PHONY: all clean install uninstall
