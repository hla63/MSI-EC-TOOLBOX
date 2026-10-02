/*
 * SSDT-MSI-KEY_FIX v5 - MSI Modern 15 A10M, VoodooPS2Controller key remap
 *
 * Keep this file pure ASCII and comments in English: macOS iasl rejects
 * sources with non-ASCII characters ("Input file does not appear to be an
 * ASL or data table source file").
 *
 * PS2 scancode -> ADB code (macOS keycode)   key            handled by
 *   e077 -> 6b                               brightness -   macOS
 *   e078 -> 71                               brightness +   macOS
 *   e071 -> 4f (79)                          F5 mic mute    agent CGEventTap
 *   e072 -> 6f (111)                         F12 rotation   agent CGEventTap
 *   e06e -> 50 (80, F19)                     F6 camera      agent CGEventTap
 *   76   -> 5a (90, F20)                     F4 touchpad    agent CGEventTap
 *   e037 -> PS2 64                           PrtSc          screenshot (F13)
 * F8 (keyboard backlight) is the standard keycode 100, no remap needed.
 *
 * v4: the camera key used ADB 0x76 (keycode 118), which is also the standard
 *     F4: Fn+F4 toggled the camera. ADB 0x50 (F19) is unused.
 * v5: the MSI F4 hotkey sends Ctrl + Win + F24 (the Windows touchpad toggle).
 *     F24 is PS2 scancode 0x76, unmapped in VoodooPS2 (ADB 0x80 = dropped);
 *     mapped to ADB 0x5A (F20).
 *
 * Keep the agent's k*KeyCode constants in sync with this table. Adding an
 * entry requires updating the Package (n) count above it: iasl rejects a
 * list longer than its declared length. Find unknown scancodes with
 * VoodooPS2's LogScanCodes property (sudo dmesg | grep "sending key").
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
                    "e071=4f",   /* F5 mic mute  -> ADB F18 (keycode 79)  */
                    "e072=6f",   /* F12 rotation -> ADB F12 (keycode 111) */
                    "e06e=50",   /* F6 camera    -> ADB F19 (keycode 80)  */
                    "76=5a"      /* F4 touchpad (Ctrl+Win+F24) -> ADB F20 (keycode 90) */
                }
            }
        })
    }
}
