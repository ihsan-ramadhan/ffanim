CFLAGS ?= -O2 -Wall -Wextra -std=gnu99
PREFIX  ?= $(HOME)/.local
DATADIR ?= $(PREFIX)/share
CFLAGS  += -DDATADIR='"$(DATADIR)"' 

ffanim: ffanim.c
	$(CC) $(CFLAGS) -o $@ $< -lm

install: ffanim
	install -Dm755 ffanim $(DESTDIR)$(PREFIX)/bin/ffanim
	install -Dm644 logo_braille $(DESTDIR)$(DATADIR)/ffanim/logo_braille
	install -Dm644 completions/ffanim.fish \
		$(DESTDIR)$(DATADIR)/fish/vendor_completions.d/ffanim.fish

test: ffanim test_pin
	./test_pin

test_pin: test_pin.c
	$(CC) $(CFLAGS) -o $@ $<

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/ffanim $(DESTDIR)$(DATADIR)/ffanim/logo_braille \
		$(DESTDIR)$(DATADIR)/fish/vendor_completions.d/ffanim.fish

clean:
	rm -f ffanim test_pin

.PHONY: install uninstall clean test
