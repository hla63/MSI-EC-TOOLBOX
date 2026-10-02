/*
 * SSDT-MSI-KEY_FIX v4
 *
 * e077 → ADB 6b (brightness down)
 * e078 → ADB 71 (brightness up)
 * e071 → ADB 4f (keycode 79)  = F5 mute mic    → CGEventTap agent
 * e072 → ADB 6f (keycode 111) = F12 rotation   → CGEventTap agent
 * e06e → ADB 50 (keycode 80)  = F6 caméra      → CGEventTap agent
 *   76 → ADB 5a (keycode 90)  = F4 trackpad    → CGEventTap agent
 *
 * v4: the camera key used ADB 0x76 (keycode 118), which is also the
 * standard F4: Fn+F4 toggled the camera. ADB 0x50 (F19) is unused.
 * v5: the MSI F4 hotkey sends Ctrl + Win + F24 (the Windows touchpad toggle);
 * F24 is PS2 scancode 0x76, unmapped in VoodooPS2 (ADB 0x80 = dropped).
 * Mapped to ADB 0x5A (F20); the agent toggles the touchpad on keycode 90.
 * Keep the agent's k*KeyCode constants in sync with this table.
 *
 * Adding an entry requires updating the Package (n) count above it; iasl
 * rejects a list longer than its declared length. Find scancodes with
 * VoodooPS2's LogScanCodes property (sudo dmesg | grep "sending key").
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
                Package (0x07)
                {
                    Package (0x00){},
                    "e077=6b",
                    "e078=71",
                    "e071=4f",   /* F5 mute mic    → ADB F14 (keycode 79)  */
                    "e072=6f",   /* F12 rotation   → ADB F13 (keycode 111) */
                    "e06e=50",   /* F6 caméra      → ADB F19 (keycode 80)  */
                    "76=5a"      /* F4 trackpad (Ctrl+Win+F24) → ADB F20 (keycode 90) */
                }
            }
        })
    }
}
