// ---------------------------------------------------------------------------
// MSIECToolboxDump.swift
// CLI — Affiche la table EC complète (256 registres) du MSI Modern 15
//
// Usage:
//   sudo MSIECToolboxDump                 # dump complet (tableau hex 16x16)
//   sudo MSIECToolboxDump --offset 0x2B   # affiche un registre précis
//   sudo MSIECToolboxDump --diff           # compare deux dumps (avant/après)
//   sudo MSIECToolboxDump --watch          # refresh automatique toutes les 2s
//   sudo MSIECToolboxDump --json           # sortie JSON (scripting)
//   sudo MSIECToolboxDump --help
//
// Prérequis : le kext MSIECToolbox doit être chargé (OpenCore).
// ---------------------------------------------------------------------------

import Foundation
import IOKit

// ── Types partagés (mirror de MSIECToolboxShared.h) ────────────────────────

// Mirror de MSIECToolboxSelector dans MSIECToolboxShared.h
enum Selector: UInt32 {
    case setMuteState   = 0
    case getMuteState   = 1   // remplacé par getAllState en pratique
    case dumpEC         = 2
    case setCameraState = 3
    case getAllState     = 4
    case readFanRPM     = 5
    case setFanMode     = 6
    case setCoolerBoost = 7
    case setShiftMode   = 8
    case getSystemState = 9
}

// ── Connexion IOKit ───────────────────────────────────────────────────────

func openConnection() -> io_connect_t? {
    let service = IOServiceGetMatchingService(
        kIOMainPortDefault,
        IOServiceMatching("MSIECToolboxDriver")
    )
    guard service != 0 else {
        fputs("❌ Erreur : service MSIECToolboxDriver introuvable.\n", stderr)
        fputs("   → Le kext est-il chargé ? (kextstat | grep MSIECToolbox)\n", stderr)
        return nil
    }
    defer { IOObjectRelease(service) }

    var conn: io_connect_t = 0
    let kr = IOServiceOpen(service, mach_task_self_, 0, &conn)
    guard kr == KERN_SUCCESS else {
        fputs(String(format: "❌ IOServiceOpen failed: 0x%08X\n", kr), stderr)
        fputs("   → Relancer avec sudo ?\n", stderr)
        return nil
    }
    return conn
}

// ── Lecture EC via kext ───────────────────────────────────────────────────

func readECDump(conn: io_connect_t) -> [UInt8]? {
    var output = [UInt8](repeating: 0, count: 256)
    var outputSize = output.count

    let kr = output.withUnsafeMutableBytes { ptr in
        IOConnectCallStructMethod(
            conn,
            Selector.dumpEC.rawValue,
            nil, 0,
            ptr.baseAddress, &outputSize
        )
    }

    guard kr == KERN_SUCCESS else {
        fputs(String(format: "❌ dumpEC failed: 0x%08X\n", kr), stderr)
        return nil
    }
    return output
}

// ── Annotations des registres connus (MSI Modern 15) ─────────────────────

// Registres EC confirmés par dump sur MSI Modern 15 A10M (firmware 1551EMS1).
// Sources : msi-ec Linux driver CONF_G1_5 + captures RW-Everything.
let knownRegisters: [Int: (name: String, desc: String)] = [
    // LED mute — confirmées (base + bit 0x04)
    0x2B: ("MIC_MUTE",      "LED mute microphone   — OFF=0x80 / ON=0x84 (bit 0x04)"),
    0x2C: ("SPK_MUTE",      "LED mute haut-parleur — OFF=0xE0 / ON=0xE4 (bit 0x04)"),
    0x2E: ("CAM_STATE",     "Caméra — actif=0x4B / coupé=0x49"),
    // Fan / performance — confirmés
    0x68: ("CPU_TEMP",      "Température CPU (°C direct)"),
    0x71: ("CPU_FAN_PCT",   "Vitesse fan CPU (%, 0–150)"),
    0x80: ("GPU_TEMP",      "Température GPU (°C direct, iGPU sur A10M)"),
    0x89: ("GPU_FAN_PCT",   "Vitesse fan GPU (% — miroir probable sur A10M iGPU)"),
    0x98: ("COOLER_BOOST",  "Cooler Boost — bit7: 0x80=ON 0x00=OFF"),
    // Fan curve CPU (6 breakpoints, °C)
    0x6A: ("FAN_TEMP0",     "Fan CPU seuil 0 (°C) — défaut 0x32=50°C"),
    0x6B: ("FAN_TEMP1",     "Fan CPU seuil 1 (°C) — défaut 0x3A=58°C"),
    0x6C: ("FAN_TEMP2",     "Fan CPU seuil 2 (°C) — défaut 0x41=65°C"),
    0x6D: ("FAN_TEMP3",     "Fan CPU seuil 3 (°C) — défaut 0x46=70°C"),
    0x6E: ("FAN_TEMP4",     "Fan CPU seuil 4 (°C) — défaut 0x5A=90°C"),
    0x6F: ("FAN_TEMP5",     "Fan CPU seuil 5 (°C) — défaut 0x5F=95°C"),
    // Fan speeds CPU (6 breakpoints, %)
    0x72: ("FAN_SPD0",      "Fan CPU vitesse 0 (%) — défaut 0x00=0%"),
    0x73: ("FAN_SPD1",      "Fan CPU vitesse 1 (%) — défaut 0x3A=58%"),
    0x74: ("FAN_SPD2",      "Fan CPU vitesse 2 (%) — défaut 0x41=65%"),
    0x75: ("FAN_SPD3",      "Fan CPU vitesse 3 (%) — défaut 0x48=72%"),
    0x76: ("FAN_SPD4",      "Fan CPU vitesse 4 (%) — défaut 0x50=80%"),
    0x77: ("FAN_SPD5",      "Fan CPU vitesse 5 (%) — défaut 0x55=85%"),
    0x78: ("FAN_SPD6",      "Fan CPU vitesse 6 (%) — fixe 0x64=100%"),
    // RPM bruts (big-endian, formule ISW)
    0xCA: ("GPU_RPM_HI",    "Fan GPU RPM octet haut (big-endian, formule ISW)"),
    0xCB: ("GPU_RPM_LO",    "Fan GPU RPM octet bas"),
    0xCC: ("CPU_RPM_HI",    "Fan CPU RPM octet haut (big-endian, formule ISW)"),
    0xCD: ("CPU_RPM_LO",    "Fan CPU RPM octet bas"),
    // Profil de performance
    0xEF: ("BAT_CHARGE",    "Seuil arrêt charge — 0x64=100% 0xBC=60% 0x50=80%"),
    0xF2: ("SHIFT_MODE",    "Shift mode — 0xC0=turbo 0xC1=confort 0xC2=éco"),
    0xF4: ("FAN_MODE",      "Fan mode — 0x0D=auto 0x1D=silent 0x8D=advanced"),
]

// ── Rendu — tableau hex 16×16 ─────────────────────────────────────────────

let ansiReset  = "\u{1B}[0m"
let ansiYellow = "\u{1B}[33m"
let ansiGreen  = "\u{1B}[32m"
let ansiRed    = "\u{1B}[31m"
let ansiBold   = "\u{1B}[1m"
let ansiDim    = "\u{1B}[2m"
let ansiCyan   = "\u{1B}[36m"

func isatty_check() -> Bool {
    return isatty(STDOUT_FILENO) != 0
}

let useColor = isatty_check()

func colored(_ s: String, _ code: String) -> String {
    useColor ? code + s + ansiReset : s
}

func renderTable(_ data: [UInt8], diff: [UInt8]? = nil) -> String {
    var out = ""

    // En-tête colonnes
    out += colored("      ", ansiBold)
    for col in 0..<16 {
        out += colored(String(format: " +%X ", col), ansiBold + ansiCyan)
    }
    out += "\n"
    out += colored(String(repeating: "─", count: 70), ansiDim) + "\n"

    for row in 0..<16 {
        let rowOffset = row * 16
        out += colored(String(format: " %02X │ ", rowOffset), ansiBold + ansiCyan)

        for col in 0..<16 {
            let idx  = rowOffset + col
            let val  = data[idx]
            let prev = diff?[idx]

            let hex = String(format: "%02X", val)

            if let prev = prev, prev != val {
                // Valeur modifiée depuis le dernier snapshot
                out += colored(hex, ansiRed + ansiBold) + " "
            } else if knownRegisters[idx] != nil {
                // Registre annoté connu
                out += colored(hex, ansiYellow) + " "
            } else if val == 0x00 {
                out += colored(hex, ansiDim) + " "
            } else {
                out += hex + " "
            }
        }

        // ASCII side-panel
        out += colored(" │ ", ansiDim)
        for col in 0..<16 {
            let c = data[rowOffset + col]
            let ch = (c >= 0x20 && c < 0x7F) ? String(UnicodeScalar(c)) : "·"
            out += colored(ch, ansiDim)
        }
        out += "\n"
    }

    return out
}

func renderLegend(_ data: [UInt8]) -> String {
    var out = "\n" + colored("── Registres annotés ──────────────────────────────────────\n", ansiDim)
    let sorted = knownRegisters.sorted { $0.key < $1.key }
    for (offset, info) in sorted {
        let val = data[offset]
        let indicator: String
        if offset == 0x2B || offset == 0x2C {
            let baseOff: UInt8 = (offset == 0x2B) ? 0x80 : 0xE0
            let baseOn:  UInt8 = baseOff | 0x04
            indicator = (val == baseOn)
                ? colored(String(format: " [0x%02X — LED ON, MUET]",  val), ansiRed)
                : colored(String(format: " [0x%02X — LED OFF, actif]", val), ansiGreen)
        } else if offset == 0x2E {
            indicator = (val == 0x49)
                ? colored(" [0x49 — COUPÉE]", ansiRed)
                : colored(" [0x4B — active]", ansiGreen)
        } else if offset == 0x98 {
            indicator = (val & 0x80) != 0
                ? colored(" [Cooler Boost ON]", ansiRed + ansiBold)
                : colored(" [Cooler Boost OFF]", ansiGreen)
        } else if offset == 0xF2 {
            let mode = val == 0xC0 ? "turbo" : val == 0xC1 ? "confort" : val == 0xC2 ? "éco" : "?"
            indicator = colored(" [\(mode)]", ansiCyan)
        } else if offset == 0xF4 {
            let mode = val == 0x0D ? "auto" : val == 0x1D ? "silent" : val == 0x8D ? "advanced" : "?"
            indicator = colored(" [\(mode)]", ansiCyan)
        } else {
            indicator = ""
        }
        out += String(format: "  %@  0x%02X  %-10@  = 0x%02X (%3d)%@\n",
            colored(String(format: "0x%02X", offset), ansiYellow),
            offset,
            colored(info.name, ansiBold),
            val, val,
            indicator)
    }
    return out
}

func renderJSON(_ data: [UInt8]) -> String {
    var dict: [String: Any] = [:]
    for i in 0..<256 {
        let key = String(format: "0x%02X", i)
        if let info = knownRegisters[i] {
            dict[key] = ["value": data[i], "name": info.name, "desc": info.desc]
        } else {
            dict[key] = ["value": data[i]]
        }
    }
    let arr = (0..<256).map { i -> [String: Any] in
        var entry: [String: Any] = ["offset": i, "value": data[i],
                                    "hex": String(format: "0x%02X", data[i])]
        if let info = knownRegisters[i] {
            entry["name"] = info.name
            entry["desc"] = info.desc
        }
        return entry
    }
    if let json = try? JSONSerialization.data(withJSONObject: arr, options: [.prettyPrinted]),
       let str = String(data: json, encoding: .utf8) {
        return str
    }
    return "{}"
}

// ── Parsing arguments ─────────────────────────────────────────────────────

struct Args {
    var offset:  Int?    = nil
    var diff:    Bool    = false
    var watch:   Bool    = false
    var json:    Bool    = false
    var interval: Double = 2.0
}

func parseArgs() -> Args? {
    var args  = Args()
    var argv  = CommandLine.arguments.dropFirst()

    while !argv.isEmpty {
        let a = argv.removeFirst()
        switch a {
        case "--help", "-h":
            printUsage(); return nil
        case "--offset", "-o":
            guard let next = argv.first,
                  let val  = Int(next.hasPrefix("0x") ? String(next.dropFirst(2)) : next,
                                 radix: next.hasPrefix("0x") ? 16 : 10)
            else { fputs("❌ --offset requiert une valeur (ex: 0x2B)\n", stderr); return nil }
            argv.removeFirst()
            args.offset = val
        case "--diff", "-d":
            args.diff = true
        case "--watch", "-w":
            args.watch = true
        case "--json", "-j":
            args.json = true
        case "--interval", "-i":
            guard let next = argv.first, let val = Double(next) else {
                fputs("❌ --interval requiert un nombre (secondes)\n", stderr); return nil
            }
            argv.removeFirst()
            args.interval = val
        default:
            fputs("❌ Argument inconnu : \(a)\n", stderr); return nil
        }
    }
    return args
}

func printUsage() {
    print("""
\(colored("MSIECToolboxDump", ansiBold)) — Dump table EC (MSI Modern 15)

\(colored("Usage:", ansiBold))
  sudo MSIECToolboxDump [options]

\(colored("Options:", ansiBold))
  \(colored("--offset, -o <hex>", ansiCyan))   Affiche un seul registre (ex: --offset 0x2B)
  \(colored("--diff,   -d", ansiCyan))          Deux dumps successifs, met en rouge les diff
  \(colored("--watch,  -w", ansiCyan))          Refresh continu (défaut: 2s, voir --interval)
  \(colored("--interval,-i <s>", ansiCyan))     Intervalle refresh en secondes (avec --watch)
  \(colored("--json,   -j", ansiCyan))          Sortie JSON (pour scripting / comparaison)
  \(colored("--help,   -h", ansiCyan))          Affiche cette aide

\(colored("Légende couleurs:", ansiBold))
  \(colored("JAUNE", ansiYellow))  Registre annoté (connu)
  \(colored("ROUGE", ansiRed))   Valeur modifiée (mode --diff / --watch)
  \(colored("Gris", ansiDim))   Valeur 0x00

\(colored("Exemples:", ansiBold))
  sudo MSIECToolboxDump                    # dump tableau complet
  sudo MSIECToolboxDump --offset 0x2B      # registre LED mute speaker
  sudo MSIECToolboxDump --watch            # surveillance en temps réel
  sudo MSIECToolboxDump --watch --interval 0.5
  sudo MSIECToolboxDump --json > ec.json   # export JSON
  sudo MSIECToolboxDump --diff             # snapshot avant/après (pause entre les deux)
""")
}

// ── Point d'entrée ────────────────────────────────────────────────────────

guard let args = parseArgs() else { exit(1) }

guard let conn = openConnection() else { exit(2) }
defer { IOServiceClose(conn) }

// ── Mode --offset ─────────────────────────────────────────────────────────
if let offset = args.offset {
    guard (0...255).contains(offset) else {
        fputs("❌ Offset invalide (0x00–0xFF)\n", stderr); exit(1)
    }
    guard let data = readECDump(conn: conn) else { exit(3) }
    let val = data[offset]
    print(String(format: "\n Offset %@ = %@ (%d / 0b%08b)\n",
        colored(String(format: "0x%02X", offset), ansiYellow + ansiBold),
        colored(String(format: "0x%02X", val), ansiBold),
        val, val))
    if let info = knownRegisters[offset] {
        print(String(format: " %@  %@\n",
            colored(info.name, ansiGreen + ansiBold), info.desc))
        if offset == 0x2B || offset == 0x2C {
            let ledOn = (val & 0x04) != 0
            print(" LED : " + (ledOn
                ? colored("ON — MUET", ansiRed + ansiBold)
                : colored("OFF — actif", ansiGreen + ansiBold)) + "\n")
        } else if offset == 0x2E {
            let off = val == 0x49
            print(" Caméra : " + (off
                ? colored("COUPÉE (0x49)", ansiRed + ansiBold)
                : colored("active (0x4B)", ansiGreen + ansiBold)) + "\n")
        } else if offset == 0x98 {
            print(" Cooler Boost : " + ((val & 0x80) != 0
                ? colored("ON", ansiRed + ansiBold)
                : colored("OFF", ansiGreen + ansiBold)) + "\n")
        }
    }
    exit(0)
}

// ── Mode --json ───────────────────────────────────────────────────────────
if args.json {
    guard let data = readECDump(conn: conn) else { exit(3) }
    print(renderJSON(data))
    exit(0)
}

// ── Mode --diff ───────────────────────────────────────────────────────────
if args.diff {
    print(colored("\n📸 Snapshot 1 — appuie sur Entrée pour capturer le snapshot 2...", ansiBold))
    guard let snap1 = readECDump(conn: conn) else { exit(3) }
    _ = readLine()
    guard let snap2 = readECDump(conn: conn) else { exit(3) }

    let changed = (0..<256).filter { snap1[$0] != snap2[$0] }

    if useColor { print("\u{1B}[2J\u{1B}[H", terminator: "") } // clear screen
    print(colored("\n MSI Modern 15 — EC Diff\n", ansiBold))
    print(renderTable(snap2, diff: snap1))
    print(renderLegend(snap2))

    if changed.isEmpty {
        print(colored("\n ✅ Aucun registre modifié entre les deux snapshots.\n", ansiGreen))
    } else {
        print(colored("\n 🔴 Registres modifiés :", ansiRed + ansiBold))
        for i in changed {
            let name = knownRegisters[i].map { " (\($0.name))" } ?? ""
            print(String(format: "   0x%02X%@  :  0x%02X → 0x%02X",
                i, name, snap1[i], snap2[i]))
        }
        print()
    }
    exit(0)
}

// ── Mode --watch ──────────────────────────────────────────────────────────
if args.watch {
    var prev: [UInt8]? = nil
    print(colored(" MSI Modern 15 — EC Watch (Ctrl+C pour quitter)\n", ansiBold))

    signal(SIGINT) { _ in
        print(colored("\n Arrêté.\n", ansiDim))
        exit(0)
    }

    while true {
        guard let data = readECDump(conn: conn) else {
            Thread.sleep(forTimeInterval: args.interval); continue
        }
        if useColor { print("\u{1B}[2J\u{1B}[H", terminator: "") }
        let ts = ISO8601DateFormatter().string(from: Date())
        print(colored(" MSI Modern 15 — EC Watch  [\(ts)]\n", ansiBold + ansiCyan))
        print(renderTable(data, diff: prev))
        print(renderLegend(data))
        prev = data
        Thread.sleep(forTimeInterval: args.interval)
    }
}

// ── Mode par défaut — dump complet ────────────────────────────────────────
guard let data = readECDump(conn: conn) else { exit(3) }
print(colored("\n MSI Modern 15 — EC Table complète\n", ansiBold + ansiCyan))
print(renderTable(data))
print(renderLegend(data))
print(colored(" \(colored("Légende:", ansiBold)) \(colored("■", ansiYellow)) registre annoté   \(colored("■", ansiDim)) valeur 0x00\n", ""))
