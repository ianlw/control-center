CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += $(shell pkg-config --cflags gtk+-3.0 gtk-layer-shell-0)
LDFLAGS += $(shell pkg-config --libs gtk+-3.0 gtk-layer-shell-0)
LDFLAGS += -pthread

SRC = src/main.c src/backend.c
BIN = control-center

PREFIX   ?= $(HOME)/.local
BINDIR   ?= $(PREFIX)/bin
CONFDIR  ?= $(HOME)/.config/control-center

all: $(BIN)

$(BIN): $(SRC) src/backend.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

run: $(BIN)
	./scripts/toggle.sh

check: $(BIN)
	./$(BIN) --check ./style.css

install: $(BIN)
	install -Dm755 $(BIN) $(BINDIR)/$(BIN)
	install -Dm644 style.css $(CONFDIR)/style.css
	install -Dm755 scripts/toggle.sh $(CONFDIR)/toggle.sh
	@echo "Instalado. Añade a Hyprland: bind = SUPER, C, exec, $(CONFDIR)/toggle.sh"
	@echo "Y registra la capa en hyprglass.lua: hg.layer(\"control-center\", { preset = \"glass-menu\", mask_threshold = 0.05 })"

clean:
	rm -f $(BIN)

.PHONY: all run check install clean
