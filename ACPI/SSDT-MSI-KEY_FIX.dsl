/*
 * SSDT-MSI-KEY_FIX v3 (original — NE PAS MODIFIER)
 *
 * e077 → ADB 6b (brightness down)
 * e078 → ADB 71 (brightness up)
 * e071 → ADB 4f (keycode 79)  = F5 mute mic    → CGEventTap agent
 * e072 → ADB 6f (keycode 111) = F12 rotation   → CGEventTap agent
 * e06e → ADB 76 (keycode 118) = F6 caméra      → CGEventTap agent
 *
 * F8 (rétroéclairage) est intercepté via keycode standard 100 dans l'agent
 * sans modification de ce SSDT — ajouter "42=6a" casse le parsing RMCF.
 */
DefinitionBlock ("", "SSDT", 2, "hack", "ps2", 0x00000000)
{
    External (_SB_.PCI0.LPCB.PS2K, DeviceObj)

    Scope (_SB.PCI0.LPCB.PS2K)
    {
        Name (RMCF, Package (0x02)
        {
            "Keyboard",
            Package (0x04)
            {
                "Custom PS2 Map",
                Package (0x02)
                {
                    Package (0x00){},
                    "e037=64"
                },

                "Custom ADB Map",
                Package (0x06)
                {
                    Package (0x00){},
                    "e077=6b",
                    "e078=71",
                    "e071=4f",   /* F5 mute mic    → ADB F14 (keycode 79)  */
                    "e072=6f",   /* F12 rotation   → ADB F13 (keycode 111) */
                    "e06e=76"    /* F6 caméra      → ADB    (keycode 118)  */
                }
            }
        })
    }
}
