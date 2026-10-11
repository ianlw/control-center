#!/bin/sh
# toggle.sh — abre/cierra el Control Center (para keybind de Hyprland)
# Uso en hyprland: bind = SUPER, C, exec, ~/.config/control-center/toggle.sh

BIN="control-center"

# 1) si ya está abierto, cerrarlo (comportamiento toggle como macOS).
# Se verifica la muerte y se escala a SIGKILL: un cierre nunca debe
# terminar abriendo otra instancia.
if pgrep -x "$BIN" >/dev/null 2>&1; then
    pkill -x "$BIN"
    for _i in 1 2 3 4 5; do
        pgrep -x "$BIN" >/dev/null 2>&1 || exit 0
        sleep 0.2
    done
    pkill -9 -x "$BIN"
    exit 0
fi

# 2) localizar el binario: mismo dir del script, ~/.local/bin o PATH
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
if [ -x "$HERE/$BIN" ]; then
    BINPATH="$HERE/$BIN"
elif [ -x "$HOME/.local/bin/$BIN" ]; then
    BINPATH="$HOME/.local/bin/$BIN"
else
    BINPATH="$(command -v "$BIN" 2>/dev/null)"
fi

if [ -z "$BINPATH" ]; then
    notify-send "Control Center" "Binario '$BIN' no encontrado. Ejecuta 'make install'." 2>/dev/null
    echo "control-center no encontrado" >&2
    exit 1
fi

# 3) el CSS vive junto al binario o en ~/.config/control-center
export CC_CSS_DIR="$HERE"
exec "$BINPATH" "$@"
