# Control Center estilo macOS Tahoe para Hyprland

Panel de control rápido clonando el **Control Center de macOS Tahoe (26/27)** a partir
de tus capturas: tarjetas redondeadas con icono + 2 líneas, botones circulares,
Now Playing alto, Stage Manager + Screen Mirroring, Focus ancho y sliders anchos
de Display y Sound.

El **liquid glass lo pone HyprGlass**: la ventana es una capa `overlay` transparente
con namespace `control-center`. El CSS incluye un fallback translúcido por si
HyprGlass está apagado.

## Requisitos

- Hyprland + `gtk3` + `gtk-layer-shell` (ya los tienes)
- Herramientas CLI estándar (todas opcionales salvo `nmcli`/`wpctl`):
  `nmcli`, `rfkill`, `bluetoothctl` (paquete `bluez-utils`, opcional),
  `wpctl`/`pactl`, `brightnessctl` o `light`, `playerctl`,
  `dunstctl` (o swaync/mako), `hyprshot`, `hyprlock`, `gsettings`
- Para compilar: `gcc`, `pkg-config`

Sin `sudo`: todo compila y se instala en `~/.local` / `~/.config`.

## Compilar y probar

```sh
make            # genera ./control-center
./control-center --check ./style.css   # verifica CSS + backends sin abrir GUI
make install    # binario a ~/.local/bin, CSS+toggle a ~/.config/control-center
```

Depuración de subpáginas sin clicar: `control-center --page wifi`
(páginas: `main wifi bt sound display focus mirror airdrop`).

## Integración con Hyprland

**1. Abrir/cerrar con una tecla** (toggle como en macOS):

```ini
bind = SUPER, C, exec, ~/.config/control-center/toggle.sh
```

**2. HyprGlass** — en `~/.config/hypr/configs/hyprglass.lua`:

```lua
hg.layer("control-center", {
    preset         = "glass-menu",
    mask_threshold = 0.05,
})
```

**3. Reglas de capa** (animación + blur de respaldo sin HyprGlass),
ver sintaxis de tu config Lua en `config/hyprland-snippets.lua`:

```lua
-- hl.layer_rule({ name = "control-center", anim = "slide" })
-- hl.layer_rule({ name = "control-center", blur = true })
```

**4. Botón en Waybar**:

```json
"custom/control": {
    "format": " 󰍜 ",
    "on-click": "~/.config/control-center/toggle.sh"
}
```

Hay más snippets listos en `config/hyprland-snippets.lua`.

## Uso (igual que en macOS)

| Tarjeta | Click en icono | Click en texto |
|---|---|---|
| Wi-Fi | enciende/apaga radio | lista de redes + conectar (pide clave) |
| Bluetooth | enciende/apaga (`rfkill`) | dispositivos emparejados (si hay `bluetoothctl`) |
| AirDrop | cicla Off → Contactos → Everyone | detalle + alternativas Linux |
| Now Playing | — | ⏮ ▶/⏸ ⏭ vía `playerctl`, "Not Playing" si no hay nada |
| Stage Manager | abre Overview de Hyprland | — |
| Duplicar pantalla | detalle de monitores | duplica la principal en el monitor tocado |
| No molestar / Silenciar | `dunstctl` / `wpctl mute` | — |
| Focus | activa No molestar | detalle Focus |
| Display | slider `brightnessctl` | detalle + modo oscuro + Night Light |
| Sound | slider `wpctl` | detalle + salidas (`pactl`) |
| Fila inferior | modo oscuro, calculadora, captura (`hyprshot`), bloqueo (`hyprlock`) | — |

Se cierra con `Esc`, clic fuera, o repitiendo el keybind.
El banner superior ("Screenshot recently", "Conectando…") imita las píldoras de macOS.

## Mapeo honesto macOS → Linux

- **AirDrop**: no existe en Linux; el interruptor es local y el detalle sugiere
  KDE Connect / Warpinator / LocalSend.
- **Bluetooth**: sin `bluez-utils` solo hay on/off por `rfkill` (se indica en la UI).
- **Stage Manager** = Overview de Hyprland (`overview:toggle`).
- **AirPlay** = selector de salidas PipeWire.
- **Focus** = pausar `dunst`.

## Estructura

```
src/main.c      UI GTK3 + layer-shell (páginas, tarjetas, tick 2 s)
src/backend.h/.c  sistema vía CLI, con LC_ALL=C (tu locale es es_PE)
style.css       tema Tahoe claro/oscuro (clases cc-light/cc-dark)
scripts/toggle.sh  abrir/cerrar (toggle, con verificación + SIGKILL)
config/hyprland-snippets.lua  keybind, hyprglass, waybar
```

## Arquitectura multi-superficie (importante)

HyprGlass calcula bordes, refracción, fresnel y aberración cromática desde el
quad de cada **superficie** Wayland (`getRoundedBoxSDF(uv)` en `Shaders.hpp`),
con el bisel como % del tamaño total. Por eso cada botón vive en su **propia
ventana ajustada a su contenido** (no fullscreen: un quad grande comparte un
solo campo de bordes y abajo solo se ve blur). La app crea: 1 catcher
transparente sin cristal (cierra al clicar fuera), 1 banner flotante,
**15 ventanas-botón** posicionadas por la tabla `GEOMS` de `src/main.c`
(medida con texto de ancho máximo para geometría estable) y 7
ventanas-detalle también ceñidas. Cada botón recibe así su propio entorno de
cristal con su propio centro y bisel proporcional, como tus terminales kitty.

## Pendiente / ideas

- Modo "Edit Controls" de Tahoe (añadir/quitar tarjetas).
- Carátula del Now Playing (`playerctl metadata mpris:artUrl`).
- Página de batería y Nocturno automático.
