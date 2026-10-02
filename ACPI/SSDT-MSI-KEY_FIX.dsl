/*
 * SSDT-MSI-KEY_FIX v4
 *
 * e077 → ADB 6b (brightness down)
 * e078 → ADB 71 (brightness up)
 * e071 → ADB 4f (keycode 79)  = F5 mute mic    → CGEventTap agent
 * e072 → ADB 6f (keycode 111) = F12 rotation   → CGEventTap agent
 * e06e → ADB 50 (keycode 80)  = F6 caméra      → CGEventTap agent
 *
 * v4: the camera key used ADB 0x76 (keycode 118), which is also the
 * standard F4: Fn+F4 toggled the camera. ADB 0x50 (F19) is unused.
 * Keep the agent's kCameraKeyCode in sync with this table.
 *
 * Only change values, never the number of entries: the package sizes must
 * match, and adding an entry ("42=6a") broke VoodooPS2's RMCF parsing.
 * F8 (rétroéclairage) est intercepté via keycode standard 100 dans l'agent.
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
                    "e06e=50"    /* F6 caméra      → ADB F19 (keycode 80)  */
                }
            }
        })
    }
}
