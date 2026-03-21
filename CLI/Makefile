# Makefile — MSIECToolboxDump CLI
# Requiert Xcode Command Line Tools : xcode-select --install

BINARY  = MSIECToolboxDump
SOURCE  = MSIECToolboxDump.swift
DESTDIR = /usr/local/bin

# Architecture universelle (Intel + Apple Silicon)
# Pour x86_64 uniquement (Hackintosh) : remplacer par -target x86_64-apple-macos14
ARCH_FLAGS = -target x86_64-apple-macos14

.PHONY: all clean install

all: $(BINARY)

$(BINARY): $(SOURCE)
	swiftc $(ARCH_FLAGS) \
	  -framework Foundation \
	  -framework IOKit \
	  -O \
	  -o $(BINARY) \
	  $(SOURCE)
	@echo "✅ Compilé : ./$(BINARY)"

install: $(BINARY)
	install -m 755 $(BINARY) $(DESTDIR)/$(BINARY)
	@echo "✅ Installé : $(DESTDIR)/$(BINARY)"

clean:
	rm -f $(BINARY)

# Test rapide (sans kext — affiche juste l'aide)
test:
	./$(BINARY) --help
