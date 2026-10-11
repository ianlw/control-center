-- =============================================================================
-- Control Center estilo macOS Tahoe — snippets de integración
-- Copia cada bloque a su archivo correspondiente.
-- =============================================================================

--------------------------------------------------------------------------------
-- 1) HyprGlass (~/.config/hypr/configs/hyprglass.lua)
--    Registra la capa "control-center" para el liquid glass real.
--------------------------------------------------------------------------------

-- hg.layer("control-center", {
--     preset         = "glass-menu",
--     mask_threshold = 0.05,
--     live_resample  = true,
--     mask_mode      = "alpha",  -- cristal solo en los botones, sin losa detrás
-- })

--------------------------------------------------------------------------------
-- 2) Keybind para abrir/cerrar (~/.config/hypr/configs/keybinds.lua)
--    Igual que en macOS: una tecla lo abre y lo cierra (toggle).
--------------------------------------------------------------------------------

-- hl.bind({ mod = "SUPER", key = "C", act = "exec", cmd = "~/.config/control-center/toggle.sh" })

--------------------------------------------------------------------------------
-- 3) Reglas de capa (~/.config/hypr/configs/rules.lua)
--    Animación de apertura + blur de respaldo si HyprGlass está apagado.
--------------------------------------------------------------------------------

-- hl.layer_rule({ name = "control-center", anim = "slide" })
-- hl.layer_rule({ name = "control-center", blur = true })

--------------------------------------------------------------------------------
-- 4) Botón en Waybar (tu waybar existente)
--    Módulo custom que abre el panel al hacer clic:
--------------------------------------------------------------------------------

-- "custom/control": {
--     "format": " 󰍜 ",
--     "tooltip-format": "Control Center",
--     "on-click": "~/.config/control-center/toggle.sh"
-- },
