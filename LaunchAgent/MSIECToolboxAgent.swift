// MSIECToolboxAgent.swift
//
// LaunchAgent — barre de menu MSI EC Toolbox
//
// Fonctions :
//   - NSStatusItem : icône mic dans la barre, menu mic/speaker/rotation/quitter
//   - CGEventTap : F5 (keycode 79) = mute mic, F12 (keycode 111) = rotation, F8 (keycode 100) = backlight
//   - CoreAudio listener : changements mute → LEDs via kext
//   - Polling EC 500ms : robustesse
//
// Registres EC (MSI Modern 15) :
//   0x2B bit2 = MICL  (1 = mic muet)
//   0x2C bit2 = SPKL  (1 = speaker muet)
//   0x2E      = caméra (0x49=off, 0x4B=on)

import Foundation
import CoreAudio
import IOKit
import CoreGraphics
import AppKit

// ---------------------------------------------------------------------------
// MARK: – Sélecteurs & types partagés
// ---------------------------------------------------------------------------

enum MSIECToolboxSelector: UInt32 {
    case setMuteState     = 0
    case getMuteState     = 1
    case dumpEC           = 2
    case setCameraState   = 3
    case getAllState       = 4
    case readFanRPM       = 5
    case setFanMode       = 6
    case setCoolerBoost   = 7
    case setShiftMode     = 8
    case getSystemState   = 9
    case setKbBacklight   = 10  // MSIKbBacklightState → EC 0xF3
    case getKbBacklight   = 11
    case setBatteryCharge = 12
    case getBatteryCharge = 13
    case setFanCurve      = 14
    case getFanCurve      = 15
}

enum FanMode: UInt8 {
    case auto_    = 0
    case silent   = 1
    case advanced = 2

    var label: String {
        switch self {
        case .auto_:    return "Auto"
        case .silent:   return "Silencieux"
        case .advanced: return "Avancé"
        }
    }
}

struct MSIMuteState {
    var speakerMuted: UInt8
    var micMuted:     UInt8
    var reserved0:    UInt8 = 0
    var reserved1:    UInt8 = 0
}

struct MSICameraState {
    var cameraOff: UInt8
    var reserved0: UInt8 = 0
    var reserved1: UInt8 = 0
    var reserved2: UInt8 = 0
}

struct MSIAllState {
    var micMuted:     UInt8
    var speakerMuted: UInt8
    var cameraOff:    UInt8
    var reserved:     UInt8 = 0
}

struct MSIFanState {
    var gpuRPM: UInt16
    var cpuRPM: UInt16
}

struct MSIFanModeState {
    var mode:      UInt8
    var reserved0: UInt8 = 0
    var reserved1: UInt8 = 0
    var reserved2: UInt8 = 0
}

struct MSICoolerBoostState {
    var enabled:   UInt8
    var reserved0: UInt8 = 0
    var reserved1: UInt8 = 0
    var reserved2: UInt8 = 0
}

struct MSISystemState {
    var cpuTempC:    UInt8
    var gpuTempC:    UInt8
    var cpuFanPct:   UInt8
    var gpuFanPct:   UInt8
    var fanMode:     UInt8
    var shiftMode:   UInt8
    var coolerBoost: UInt8
    var reserved:    UInt8 = 0
}

struct MSIBatteryChargeState {
    var percent:   UInt8
    var reserved0: UInt8 = 0
    var reserved1: UInt8 = 0
    var reserved2: UInt8 = 0
}

struct MSIFanCurve {
    var temps:  (UInt8,UInt8,UInt8,UInt8,UInt8,UInt8) = (50,58,65,70,90,95)
    var speeds: (UInt8,UInt8,UInt8,UInt8,UInt8,UInt8) = (0,58,65,72,80,85)

    static let firmware = MSIFanCurve()

    var tempsArray:  [UInt8] { [temps.0,  temps.1,  temps.2,  temps.3,  temps.4,  temps.5]  }
    var speedsArray: [UInt8] { [speeds.0, speeds.1, speeds.2, speeds.3, speeds.4, speeds.5] }
}

struct MSIKbBacklightState {
    var level:     UInt8
    var reserved0: UInt8 = 0
    var reserved1: UInt8 = 0
    var reserved2: UInt8 = 0
}

// Registres EC (référence — utilisés dans le kext C++, pas en Swift)
// kECOffsetMic=0x2B  kECOffsetSpeaker=0x2C  kECOffsetCamera=0x2E
// kECCameraOff=0x49  kECBitLED=0x04

private let kF14KeyCode: CGKeyCode = 79   // F5 mute mic    (e071→ADB 4f)
private let kF13KeyCode: CGKeyCode = 111  // F12 rotation   (e072→ADB 6f)
private let kF6KeyCode:  CGKeyCode = 118  // F6 caméra      (e06e→ADB 76)
private let kF8KeyCode:  CGKeyCode = 100  // F8 backlight   (keycode standard macOS)

// ---------------------------------------------------------------------------
// MARK: – Profils de courbe fan
// ---------------------------------------------------------------------------

struct FanProfile: Codable {
    var name:   String
    var temps:  [UInt8]
    var speeds: [UInt8]

    func toCurve() -> MSIFanCurve {
        var c = MSIFanCurve()
        if temps.count == 6 && speeds.count == 6 {
            c.temps  = (temps[0],  temps[1],  temps[2],  temps[3],  temps[4],  temps[5])
            c.speeds = (speeds[0], speeds[1], speeds[2], speeds[3], speeds[4], speeds[5])
        }
        return c
    }

    static func fromCurve(_ curve: MSIFanCurve, name: String) -> FanProfile {
        FanProfile(name: name, temps: curve.tempsArray, speeds: curve.speedsArray)
    }
}

final class FanProfileManager {
    static let shared = FanProfileManager()
    private let udKey = "fan_profiles"
    private(set) var profiles: [FanProfile] = []

    private init() { load() }

    private func load() {
        guard let data = UserDefaults.standard.data(forKey: udKey),
              let decoded = try? JSONDecoder().decode([FanProfile].self, from: data)
        else { profiles = []; return }
        profiles = decoded
    }

    private func save() {
        guard let data = try? JSONEncoder().encode(profiles) else { return }
        UserDefaults.standard.set(data, forKey: udKey)
    }

    func add(_ profile: FanProfile) {
        profiles.removeAll { $0.name == profile.name }
        profiles.append(profile)
        save()
    }

    func delete(name: String) {
        profiles.removeAll { $0.name == name }
        save()
    }

    func profile(named name: String) -> FanProfile? {
        profiles.first { $0.name == name }
    }
}

// Classe dédiée pour le contexte IOKit — les tuples Swift ne peuvent pas être
// castés en AnyObject pour les callbacks IOKit.
private final class WatchBox {
    let client:       MSIECToolboxClient
    let onConnect:    () -> Void
    let onDisconnect: () -> Void
    init(_ c: MSIECToolboxClient, _ onC: @escaping () -> Void, _ onD: @escaping () -> Void) {
        client = c; onConnect = onC; onDisconnect = onD
    }
}

final class MSIECToolboxClient {

    private var connection:      io_connect_t = 0
    private var notifyPort:      IONotificationPortRef? = nil
    private var addedIterator:   io_iterator_t = 0
    private var removedIterator: io_iterator_t = 0
    private var watchCtx:        Unmanaged<WatchBox>? = nil

    var isConnected: Bool { connection != 0 }

    func connect() -> Bool {
        guard !isConnected else { return true }
        let service = IOServiceGetMatchingService(
            kIOMainPortDefault, IOServiceMatching("MSIECToolboxDriver"))
        guard service != 0 else { return false }
        defer { IOObjectRelease(service) }
        var conn: io_connect_t = 0
        let kr = IOServiceOpen(service, mach_task_self_, 0, &conn)
        guard kr == KERN_SUCCESS else { return false }
        connection = conn
        NSLog("[MSIECToolboxAgent] Connecté au kext (conn=0x%X)", conn)
        return true
    }

    func disconnect() {
        guard isConnected else { return }
        IOServiceClose(connection)
        connection = 0
    }

    func setMuteState(speaker: Bool, mic: Bool) -> Bool {
        guard isConnected else { return false }
        var state = MSIMuteState(speakerMuted: speaker ? 1 : 0,
                                 micMuted:     mic     ? 1 : 0)
        let kr = withUnsafeBytes(of: &state) { ptr in
            IOConnectCallStructMethod(
                connection, MSIECToolboxSelector.setMuteState.rawValue,
                ptr.baseAddress, MemoryLayout<MSIMuteState>.size, nil, nil)
        }
        return kr == KERN_SUCCESS
    }

    func setCameraState(cameraOff: Bool) -> Bool {
        guard isConnected else { return false }
        var state = MSICameraState(cameraOff: cameraOff ? 1 : 0)
        let kr = withUnsafeBytes(of: &state) { ptr in
            IOConnectCallStructMethod(
                connection, MSIECToolboxSelector.setCameraState.rawValue,
                ptr.baseAddress, MemoryLayout<MSICameraState>.size, nil, nil)
        }
        if kr != KERN_SUCCESS {
            NSLog("[MSIECToolboxAgent] setCameraState failed: 0x%08X", kr)
        }
        return kr == KERN_SUCCESS
    }

    func readFanRPM() -> (cpuRPM: Int, gpuRPM: Int)? {
        guard isConnected else { return nil }
        var out   = MSIFanState(gpuRPM: 0, cpuRPM: 0)
        var outSz = MemoryLayout<MSIFanState>.size
        let kr = withUnsafeMutableBytes(of: &out) { ptr in
            IOConnectCallStructMethod(
                connection, MSIECToolboxSelector.readFanRPM.rawValue,
                nil, 0, ptr.baseAddress, &outSz)
        }
        guard kr == KERN_SUCCESS else { return nil }
        return (cpuRPM: Int(out.cpuRPM), gpuRPM: Int(out.gpuRPM))
    }

    // ── Nouvelles méthodes fan/performance ───────────────────────────────────

    @discardableResult
    func setFanMode(_ mode: FanMode) -> Bool {
        guard isConnected else { return false }
        var state = MSIFanModeState(mode: mode.rawValue)
        let kr = withUnsafeBytes(of: &state) { ptr in
            IOConnectCallStructMethod(
                connection, MSIECToolboxSelector.setFanMode.rawValue,
                ptr.baseAddress, MemoryLayout<MSIFanModeState>.size, nil, nil)
        }
        if kr != KERN_SUCCESS { NSLog("[MSIECToolboxAgent] setFanMode \(mode.label) failed: 0x%08X", kr) }
        return kr == KERN_SUCCESS
    }

    @discardableResult
    func setCoolerBoost(_ enable: Bool) -> Bool {
        guard isConnected else { return false }
        var state = MSICoolerBoostState(enabled: enable ? 1 : 0)
        let kr = withUnsafeBytes(of: &state) { ptr in
            IOConnectCallStructMethod(
                connection, MSIECToolboxSelector.setCoolerBoost.rawValue,
                ptr.baseAddress, MemoryLayout<MSICoolerBoostState>.size, nil, nil)
        }
        if kr != KERN_SUCCESS { NSLog("[MSIECToolboxAgent] setCoolerBoost failed: 0x%08X", kr) }
        return kr == KERN_SUCCESS
    }

    @discardableResult
    func setKbBacklight(level: UInt8) -> Bool {
        guard isConnected else { return false }
        var state = MSIKbBacklightState(level: level)
        let kr = withUnsafeBytes(of: &state) { ptr in
            IOConnectCallStructMethod(
                connection, MSIECToolboxSelector.setKbBacklight.rawValue,
                ptr.baseAddress, MemoryLayout<MSIKbBacklightState>.size, nil, nil)
        }
        if kr != KERN_SUCCESS { NSLog("[MSIECToolboxAgent] setKbBacklight %d failed: 0x%08X", level, kr) }
        return kr == KERN_SUCCESS
    }

    func getKbBacklight() -> UInt8? {
        guard isConnected else { return nil }
        var out   = MSIKbBacklightState(level: 0)
        var outSz = MemoryLayout<MSIKbBacklightState>.size
        let kr = withUnsafeMutableBytes(of: &out) { ptr in
            IOConnectCallStructMethod(
                connection, MSIECToolboxSelector.getKbBacklight.rawValue,
                nil, 0, ptr.baseAddress, &outSz)
        }
        return kr == KERN_SUCCESS ? out.level : nil
    }

    func setBatteryCharge(percent: UInt8) -> Bool {
        guard isConnected else { return false }
        var state = MSIBatteryChargeState(percent: percent)
        let kr = withUnsafeBytes(of: &state) { ptr in
            IOConnectCallStructMethod(connection, MSIECToolboxSelector.setBatteryCharge.rawValue,
                ptr.baseAddress, MemoryLayout<MSIBatteryChargeState>.size, nil, nil)
        }
        if kr != KERN_SUCCESS { NSLog("[MSIECToolboxAgent] setBatteryCharge %d%% failed: 0x%08X", percent, kr) }
        return kr == KERN_SUCCESS
    }

    func getBatteryCharge() -> UInt8? {
        guard isConnected else { return nil }
        var out   = MSIBatteryChargeState(percent: 100)
        var outSz = MemoryLayout<MSIBatteryChargeState>.size
        let kr = withUnsafeMutableBytes(of: &out) { ptr in
            IOConnectCallStructMethod(connection, MSIECToolboxSelector.getBatteryCharge.rawValue,
                nil, 0, ptr.baseAddress, &outSz)
        }
        return kr == KERN_SUCCESS ? out.percent : nil
    }

    func setFanCurve(_ curve: MSIFanCurve) -> Bool {
        guard isConnected else { return false }
        var c = curve
        let kr = withUnsafeBytes(of: &c) { ptr in
            IOConnectCallStructMethod(connection, MSIECToolboxSelector.setFanCurve.rawValue,
                ptr.baseAddress, MemoryLayout<MSIFanCurve>.size, nil, nil)
        }
        if kr != KERN_SUCCESS { NSLog("[MSIECToolboxAgent] setFanCurve failed: 0x%08X", kr) }
        return kr == KERN_SUCCESS
    }

    func getFanCurve() -> MSIFanCurve? {
        guard isConnected else { return nil }
        var out   = MSIFanCurve()
        var outSz = MemoryLayout<MSIFanCurve>.size
        let kr = withUnsafeMutableBytes(of: &out) { ptr in
            IOConnectCallStructMethod(connection, MSIECToolboxSelector.getFanCurve.rawValue,
                nil, 0, ptr.baseAddress, &outSz)
        }
        return kr == KERN_SUCCESS ? out : nil
    }

    func readSystemState() -> MSISystemState? {
        guard isConnected else { return nil }
        var out   = MSISystemState(cpuTempC: 0, gpuTempC: 0, cpuFanPct: 0,
                                   gpuFanPct: 0, fanMode: 0, shiftMode: 0, coolerBoost: 0)
        var outSz = MemoryLayout<MSISystemState>.size
        let kr = withUnsafeMutableBytes(of: &out) { ptr in
            IOConnectCallStructMethod(
                connection, MSIECToolboxSelector.getSystemState.rawValue,
                nil, 0, ptr.baseAddress, &outSz)
        }
        guard kr == KERN_SUCCESS else { return nil }
        return out
    }

    // FIX PERF : utilise kMSIGetAllState — lit seulement 3 registres EC
    // au lieu de dumpEC (256 lectures raw I/O toutes les 500ms)
    func dumpEC() -> [UInt8]? {
        guard isConnected else { return nil }
        // Allouer 256 bytes directement — MSIECDump est un struct C non constructible en Swift
        var buffer = [UInt8](repeating: 0, count: 256)
        let kr = buffer.withUnsafeMutableBytes { ptr -> kern_return_t in
            var sz = ptr.count
            return IOConnectCallStructMethod(
                connection, MSIECToolboxSelector.dumpEC.rawValue,
                nil, 0, ptr.baseAddress, &sz)
        }
        guard kr == KERN_SUCCESS else { return nil }
        return buffer
    }

        func readECMuteState() -> (micMuted: Bool, speakerMuted: Bool, cameraOff: Bool)? {
        guard isConnected else { return nil }
        var out    = MSIAllState(micMuted: 0, speakerMuted: 0, cameraOff: 0, reserved: 0)
        var outSz  = MemoryLayout<MSIAllState>.size
        let kr = withUnsafeMutableBytes(of: &out) { ptr in
            IOConnectCallStructMethod(
                connection, MSIECToolboxSelector.getAllState.rawValue,
                nil, 0, ptr.baseAddress, &outSz)
        }
        guard kr == KERN_SUCCESS else { return nil }
        return (micMuted: out.micMuted != 0,
                speakerMuted: out.speakerMuted != 0,
                cameraOff: out.cameraOff != 0)
    }

    // FIX LEAK : le ctx est retenu pour la durée de vie du port (qui est infinie
    // tant que le process tourne) — on utilise passUnretained + on garde
    // une référence forte via la closure capturée dans le tuple.
    func watchService(onConnect: @escaping () -> Void, onDisconnect: @escaping () -> Void) {
        notifyPort = IONotificationPortCreate(kIOMainPortDefault)
        guard let port = notifyPort else { return }
        let source = IONotificationPortGetRunLoopSource(port).takeUnretainedValue()
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .defaultMode)

        let box = WatchBox(self, onConnect, onDisconnect)
        watchCtx = Unmanaged.passRetained(box)

        // mais un guard explicite rend l'intention claire et évite un crash silencieux
        // si la logique évolue.
        guard let ctx = watchCtx?.toOpaque() else { return }

        IOServiceAddMatchingNotification(
            port, kIOMatchedNotification,
            IOServiceMatching("MSIECToolboxDriver"),
            { rawCtx, it in
                let b = Unmanaged<WatchBox>.fromOpaque(rawCtx!).takeUnretainedValue()
                while IOIteratorNext(it) != 0 {}
                _ = b.client.connect(); b.onConnect()
            }, ctx, &addedIterator)
        while IOIteratorNext(addedIterator) != 0 {}

        IOServiceAddMatchingNotification(
            port, kIOTerminatedNotification,
            IOServiceMatching("MSIECToolboxDriver"),
            { rawCtx, it in
                let b = Unmanaged<WatchBox>.fromOpaque(rawCtx!).takeUnretainedValue()
                while IOIteratorNext(it) != 0 {}
                b.client.disconnect(); b.onDisconnect()
            }, ctx, &removedIterator)
        while IOIteratorNext(removedIterator) != 0 {}
    }

    func stopWatching() {
        watchCtx?.release()
        watchCtx = nil
        if addedIterator   != 0 { IOObjectRelease(addedIterator);   addedIterator   = 0 }
        if removedIterator != 0 { IOObjectRelease(removedIterator); removedIterator = 0 }
        if let port = notifyPort {
            let src = IONotificationPortGetRunLoopSource(port).takeUnretainedValue()
            CFRunLoopRemoveSource(CFRunLoopGetMain(), src, .defaultMode)
            IONotificationPortDestroy(port)
            notifyPort = nil
        }
    }
}

// ---------------------------------------------------------------------------
// MARK: – Barre de menu
// ---------------------------------------------------------------------------

final class MenuBarController {
    var statusItem:  NSStatusItem!
    private var micItem:     NSMenuItem!
    private var speakerItem: NSMenuItem!
    private var camItem:     NSMenuItem!
    private var rotItem:     NSMenuItem!
    private var cpuFanItem:  NSMenuItem!
    private var gpuFanItem:  NSMenuItem!
    // Nouveaux items — fan mode
    private var fanModeAutoItem:     NSMenuItem!
    private var fanModeSilentItem:   NSMenuItem!
    private var fanModeAdvItem:      NSMenuItem!
    // Cooler Boost + temp
    private var coolerBoostItem:     NSMenuItem!
    private var cpuTempItem:         NSMenuItem!
    // Dump EC
    private var ecDumpItem: NSMenuItem!
    // Batterie
    private var batteryLimitItem:         NSMenuItem!
    private(set) var currentBatteryLimit: UInt8 = 100
    // Rétroéclairage clavier
    private var kbBacklightOffItem:  NSMenuItem!
    private var kbBacklightLowItem:  NSMenuItem!
    private var kbBacklightMedItem:  NSMenuItem!
    private var kbBacklightHighItem: NSMenuItem!

    // ── Préférences — icône barre de menus ───────────────────────────────────
    // "led" (défaut) = LEDs colorées dynamiques
    // "ec"           = badge "EC" fixe
    var prefIconStyle    = "led"
    var prefRotationMode = "180"  // "180" ou "90cycle"
    var prefShowOSD      = true

    // ── Préférences — conditions de changement de couleur LED ────────────────
    // Vert  : aucun des états surveillés n'est actif
    // Orange: mic ou speaker muet (si surveillé)
    // Rouge : mic ET speaker muets (si surveillés), ou caméra coupée (si surveillée)
    var prefLEDWatchMic:    Bool = true
    var prefLEDWatchSpk:    Bool = true
    // "rouge" | "orange" | "jaune" | "none"
    var prefLEDCamColor:    String = "rouge"

    // ── Préférences — sections visibles ──────────────────────────────────────
    // Persistées dans UserDefaults sous "pref_show_<section>"
    var prefShowAudio      = true
    var prefShowMonitoring = true
    var prefShowFan        = true
    var prefShowBattery    = true
    var prefShowRotation   = true
    var prefShowBacklight  = true

    // Groupes d'items pour masquage/affichage
    private var audioItems:      [NSMenuItem] = []
    private var monitoringItems: [NSMenuItem] = []
    private var fanItems:        [NSMenuItem] = []
    private var batteryItems:    [NSMenuItem] = []
    private var rotationItems:   [NSMenuItem] = []
    private var backlightItems:  [NSMenuItem] = []

    // Séparateurs associés aux sections
    private var sepAfterAudio:     NSMenuItem!
    private var sepAfterMonitor:   NSMenuItem!
    private var sepAfterFan:       NSMenuItem!
    private var sepAfterBattery:   NSMenuItem!
    private var sepAfterRotation:  NSMenuItem!
    private var sepAfterBacklight: NSMenuItem!

    var onToggleMic:      (() -> Void)?
    var onToggleSpeaker:  (() -> Void)?
    var onToggleCamera:   (() -> Void)?
    var onToggleRotation: (() -> Void)?
    var onSetFanMode:     ((FanMode)   -> Void)?
    var onToggleCoolerBoost:  (() -> Void)?
    var onToggleBatteryLimit: (() -> Void)?
    var onOpenFanCurve:       (() -> Void)?
    var onSetKbBacklight:   ((UInt8) -> Void)?
    var onRequestECDump:    (() -> Void)?
    var onRequestPreferences: (() -> Void)?
    private(set) var currentKbBacklightLevel: UInt8 = 0

    // État courant
    private var micMuted     = false
    private var speakerMuted = false
    var headphoneConnected = false  // mis à jour par AppController.refresh()
    private var cameraActive = true
    var currentFanMode   = FanMode.auto_
    private var coolerBoostOn    = false
    var currentCoolerBoostOn: Bool { coolerBoostOn }

    func setup() {
        // Charger les préférences sauvegardées
        let ud = UserDefaults.standard
        prefIconStyle      = ud.string(forKey: "pref_icon_style")       ?? "led"
        prefRotationMode   = ud.string(forKey: "pref_rotation_mode")    ?? "180"
        prefShowOSD        = ud.object(forKey: "pref_show_osd")         as? Bool ?? true
        prefLEDWatchMic    = ud.object(forKey: "pref_led_watch_mic") as? Bool ?? true
        prefLEDWatchSpk    = ud.object(forKey: "pref_led_watch_spk") as? Bool ?? true
        prefLEDCamColor    = ud.string(forKey: "pref_led_cam_color") ?? "rouge"
        prefShowAudio      = ud.object(forKey: "pref_show_audio")      as? Bool ?? true
        prefShowMonitoring = ud.object(forKey: "pref_show_monitoring")  as? Bool ?? true
        prefShowFan        = ud.object(forKey: "pref_show_fan")         as? Bool ?? true
        prefShowBattery    = ud.object(forKey: "pref_show_battery")     as? Bool ?? true
        prefShowRotation   = ud.object(forKey: "pref_show_rotation")    as? Bool ?? true
        prefShowBacklight  = ud.object(forKey: "pref_show_backlight")   as? Bool ?? true

        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)

        buildMenu()

        // Forcer le premier dessin de l'icône après que le menu est prêt
        // Réinitialiser le cache pour que updateLED() ne soit pas bloqué par le guard
        lastLEDMic = !micMuted
        lastLEDSpk = !speakerMuted
        lastLEDCam = !cameraActive
        if prefIconStyle == "ec" {
            statusItem.length = 28
            statusItem.button?.image = makeECBadgeImage()
            statusItem.button?.imageScaling = .scaleProportionallyDown
        } else {
            updateLED()
        }
    }

    private func buildMenu() {
        let menu = NSMenu()

        micItem     = makeItem(title: "Mic : Actif",        sfSymbol: "mic.fill",            action: #selector(tapMic))
        speakerItem = makeItem(title: "Speaker : Actif",    sfSymbol: "speaker.wave.2.fill",  action: #selector(tapSpeaker))
        camItem     = makeItem(title: "Caméra : Active",    sfSymbol: "camera.fill",          action: #selector(tapCamera))
        rotItem     = makeItem(title: "Rotation : 0°",      sfSymbol: "rotate.right.fill",    action: #selector(tapRotation))
        camItem.isEnabled = true

        // Fans + température (lecture seule)
        cpuFanItem = makeItem(title: "CPU Fan : — RPM", sfSymbol: "cpu", action: nil)
        gpuFanItem = makeItem(title: "GPU Fan : — RPM", sfSymbol: "fan.fill", action: nil)
        cpuTempItem = makeItem(title: "CPU : — °C", sfSymbol: "thermometer.medium", action: nil)
        cpuFanItem.isEnabled  = false
        gpuFanItem.isEnabled  = false
        cpuTempItem.isEnabled = false

        // ── Mode ventilation ─────────────────────────────────────────────────
        let fanHeader = NSMenuItem(title: "Mode ventilation", action: nil, keyEquivalent: "")
        fanHeader.isEnabled = false
        fanModeAutoItem   = makeItem(title: FanMode.auto_.label,   sfSymbol: "fanblades",       action: #selector(tapFanAuto))
        fanModeSilentItem = makeItem(title: FanMode.silent.label,  sfSymbol: "fanblades.slash",  action: #selector(tapFanSilent))
        fanModeAdvItem    = makeItem(title: FanMode.advanced.label, sfSymbol: "slider.horizontal.3", action: #selector(tapFanAdvanced))
        updateFanModeItems(mode: .auto_)

        // Cooler Boost
        coolerBoostItem = makeItem(title: "Cooler Boost : OFF", sfSymbol: "flame", action: #selector(tapCoolerBoost))

        // ── Batterie ──────────────────────────────────────────────────────────
        batteryLimitItem = makeItem(title: "Charge : 100%", sfSymbol: "battery.100", action: #selector(tapBatteryLimit))

        // ── Assemblage du menu ───────────────────────────────────────────────
        audioItems = [micItem, speakerItem, camItem]
        menu.addItem(micItem)
        menu.addItem(speakerItem)
        menu.addItem(camItem)
        sepAfterAudio = NSMenuItem.separator()
        menu.addItem(sepAfterAudio)
        ecDumpItem = makeItem(title: "Table EC", sfSymbol: "tablecells", action: #selector(tapECDump))
        monitoringItems = [cpuFanItem, gpuFanItem, cpuTempItem, ecDumpItem]
        menu.addItem(cpuFanItem)
        menu.addItem(gpuFanItem)
        menu.addItem(cpuTempItem)
        menu.addItem(ecDumpItem)
        sepAfterMonitor = NSMenuItem.separator()
        menu.addItem(sepAfterMonitor)
        menu.addItem(fanHeader)
        menu.addItem(fanModeAutoItem)
        menu.addItem(fanModeSilentItem)
        let fanCurveItem = makeItem(title: "  Modifier la courbe...", sfSymbol: "waveform.path", action: #selector(tapFanCurve))
        fanCurveItem.indentationLevel = 1
        menu.addItem(fanModeAdvItem)
        menu.addItem(fanCurveItem)
        fanItems = [fanHeader, fanModeAutoItem, fanModeSilentItem, fanModeAdvItem, fanCurveItem, coolerBoostItem]
        menu.addItem(coolerBoostItem)
        sepAfterFan = NSMenuItem.separator()
        menu.addItem(sepAfterFan)
        batteryItems = [batteryLimitItem]
        menu.addItem(batteryLimitItem)
        sepAfterBattery = NSMenuItem.separator()
        menu.addItem(sepAfterBattery)
        rotationItems = [rotItem]
        menu.addItem(rotItem)
        sepAfterRotation = NSMenuItem.separator()
        menu.addItem(sepAfterRotation)

        // ── Rétroéclairage clavier ─────────────────────────────────────────────
        let kbHeader = NSMenuItem(title: "Rétroéclairage clavier", action: nil, keyEquivalent: "")
        kbHeader.isEnabled = false
        kbBacklightOffItem  = makeItem(title: "Off",    sfSymbol: "keyboard",      action: #selector(tapKbOff))
        kbBacklightLowItem  = makeItem(title: "Faible", sfSymbol: "keyboard",      action: #selector(tapKbLow))
        kbBacklightMedItem  = makeItem(title: "Moyen",  sfSymbol: "keyboard.fill", action: #selector(tapKbMed))
        kbBacklightHighItem = makeItem(title: "Élevé",  sfSymbol: "keyboard.fill", action: #selector(tapKbHigh))
        updateKbBacklightItems(level: 0)

        menu.addItem(kbHeader)
        menu.addItem(kbBacklightOffItem)
        menu.addItem(kbBacklightLowItem)
        menu.addItem(kbBacklightMedItem)
        backlightItems = [kbHeader, kbBacklightOffItem, kbBacklightLowItem, kbBacklightMedItem, kbBacklightHighItem]
        menu.addItem(kbBacklightHighItem)
        sepAfterBacklight = NSMenuItem.separator()
        menu.addItem(sepAfterBacklight)

        // ── Item Préférences → panel ─────────────────────────────────────
        let prefItem = makeItem(title: "Préférences...", sfSymbol: "gearshape", action: #selector(tapPreferences))
        prefItem.keyEquivalent = ","
        menu.addItem(prefItem)
        menu.addItem(NSMenuItem.separator())

        // Appliquer les préférences initiales
        applyVisibilityPrefs()

        let quit = makeItem(title: "Quitter MSIECToolbox", sfSymbol: "power", action: #selector(tapQuit))
        quit.keyEquivalent = "q"
        menu.addItem(quit)

        statusItem.menu = menu
    }

    // ── LED dessinée programmatiquement ──────────────────────────────────────
    // Layout barre :
    //   Tout OK           → 🟢  (une LED verte)
    //   Un muet           → 🟠 mic|spk  (label du muet)
    //   Les deux muets    → 🔴  (une LED rouge, pas de label)
    //   + caméra active   → toujours + 🔴 cam  à droite

    private enum LEDColor { case green, orange, red, yellow }

    private func makeStatusImage() -> NSImage {
        if prefIconStyle == "ec" {
            return makeECBadgeImage()
        }
        // Utiliser les variables watched* pour largeur ET rendu (cohérence avec les prefs)
        let watchedMicMuted = prefLEDWatchMic && micMuted
        let watchedSpkMuted = prefLEDWatchSpk && speakerMuted
        let watchedCamOff   = prefLEDCamColor != "none" && !cameraActive

        let watchedBothMuted    = watchedMicMuted && watchedSpkMuted
        let watchedOnlyOneMuted = (watchedMicMuted || watchedSpkMuted) && !watchedBothMuted

        // Width = 42 only when an audio label (mic/spk/out) is needed.
        // Camera state uses color only (yellow/orange/red) — no label — so it
        // stays in the same 18x18 frame as the green/orange/red indicators.
        let width: CGFloat = watchedOnlyOneMuted ? 42 : 18
        let img = NSImage(size: CGSize(width: width, height: 18))
        img.lockFocus()
        NSColor.clear.setFill()
        NSBezierPath(rect: NSRect(x: 0, y: 0, width: width, height: 18)).fill()

        // ── Règles couleur LED ───────────────────────────────────────────
        // Rouge  : mic+spk muets ET (caméra off surveillée OU caméra "none")
        // Orange : un seul audio mauvais, OU caméra seule (couleur = pref caméra)
        // Vert   : aucun état mauvais

        let camNone   = prefLEDCamColor == "none"
        let isRouge   = watchedBothMuted && (watchedCamOff || camNone)

        // Camera-only bad state: color encodes the severity (yellow/orange/red).
        // No text label — keeps the icon the same 18x18 size as other states.
        let camOnlyOff = watchedCamOff && !watchedMicMuted && !watchedSpkMuted

        let ledColor: LEDColor
        if isRouge {
            ledColor = .red
        } else if camOnlyOff {
            ledColor = (prefLEDCamColor == "jaune") ? .yellow
                     : (prefLEDCamColor == "orange") ? .orange : .red
        } else if watchedMicMuted || watchedSpkMuted || watchedCamOff {
            ledColor = .orange
        } else {
            ledColor = .green
        }

        drawLED(at: CGPoint(x: 9, y: 9), color: ledColor, label: nil)
        if watchedOnlyOneMuted {
            drawLabel(watchedMicMuted ? "mic" : (headphoneConnected ? "out" : "spk"), x: 18,
                      color: NSColor(red: 1.0, green: 0.6, blue: 0.1, alpha: 1.0))
        }

        img.unlockFocus()
        img.isTemplate = false
        return img
    }

    private func makeECBadgeImage() -> NSImage {
        // Valise contour + "EC" transparent + clé à molette à droite
        // isTemplate=true → couleur adaptée light/dark + vibrancy automatique
        let W: CGFloat = 24
        let H: CGFloat = 18
        let img = NSImage(size: CGSize(width: W, height: H))
        img.lockFocus()

        let lw: CGFloat = 1.1

        // ── Corps valise (contour uniquement, fond transparent) ───────────
        let bX: CGFloat = 1,  bY: CGFloat = 1.5
        let bW: CGFloat = 19, bH: CGFloat = 11
        let body = NSBezierPath(roundedRect: NSRect(x: bX, y: bY, width: bW, height: bH),
                                 xRadius: 2, yRadius: 2)
        NSColor.black.setStroke()
        body.lineWidth = lw
        body.stroke()

        // ── Poignée (arche au-dessus, contour uniquement) ─────────────────
        let hW: CGFloat = 7, hH: CGFloat = 3
        let hX = bX + (bW - hW) / 2
        let hY = bY + bH - 0.5
        let handle = NSBezierPath()
        handle.move(to: CGPoint(x: hX, y: hY))
        handle.appendArc(withCenter: CGPoint(x: hX + hW / 2, y: hY + hH / 2),
                         radius: hW / 2,
                         startAngle: 180, endAngle: 0,
                         clockwise: true)
        handle.line(to: CGPoint(x: hX + hW, y: hY))
        NSColor.black.setStroke()
        handle.lineWidth = lw
        handle.stroke()

        // ── Texte "EC" centré dans le corps ──────────────────────────────
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.boldSystemFont(ofSize: 7),
            .foregroundColor: NSColor.black
        ]
        let str = NSAttributedString(string: "EC", attributes: attrs)
        let sz = str.size()
        str.draw(at: CGPoint(x: bX + (bW - sz.width) / 2,
                             y: bY + (bH - sz.height) / 2))

        img.unlockFocus()
        img.isTemplate = true
        return img
    }

    private func drawLED(at center: CGPoint, color: LEDColor, label: String?) {
        let nsColor: NSColor
        switch color {
        case .green:  nsColor = NSColor(red: 0.2,  green: 0.85, blue: 0.3,  alpha: 1.0)
        case .orange: nsColor = NSColor(red: 1.0,  green: 0.6,  blue: 0.05, alpha: 1.0)
        case .red:    nsColor = NSColor(red: 1.0,  green: 0.2,  blue: 0.2,  alpha: 1.0)
        case .yellow: nsColor = NSColor(red: 0.95, green: 0.8,  blue: 0.0,  alpha: 1.0)
        }
        let rect = NSRect(x: center.x - 6, y: center.y - 6, width: 12, height: 12)
        nsColor.setFill()
        NSBezierPath(ovalIn: rect).fill()
        // Reflet
        NSColor.white.withAlphaComponent(0.45).setFill()
        NSBezierPath(ovalIn: NSRect(x: center.x - 3.5, y: center.y + 0.5, width: 3, height: 3)).fill()
        // Label interne optionnel
        if let label = label {
            let attrs: [NSAttributedString.Key: Any] = [
                .font: NSFont.boldSystemFont(ofSize: 8),
                .foregroundColor: NSColor.white
            ]
            let str = NSAttributedString(string: label, attributes: attrs)
            let sz  = str.size()
            str.draw(at: CGPoint(x: center.x - sz.width / 2, y: center.y - sz.height / 2))
        }
    }

    private func drawLabel(_ text: String, x: CGFloat, color: NSColor) {
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 8, weight: .medium),
            .foregroundColor: color
        ]
        let str = NSAttributedString(string: text, attributes: attrs)
        str.draw(at: CGPoint(x: x, y: 9 - str.size().height / 2))
    }

    // FIX PERF : cache de l'état précédent — évite de redessiner l'image
    // si l'état n'a pas changé (updateLED() peut être appelé très fréquemment)
    private var lastLEDMic     = false
    private var lastLEDSpk     = false
    private var lastLEDCam     = true

    func updateLED() {
        DispatchQueue.main.async {
            // Redessiner seulement si l'état a changé
            let wMic = self.prefLEDWatchMic && self.micMuted
            let wSpk = self.prefLEDWatchSpk && self.speakerMuted
            let wCam = self.prefLEDCamColor != "none" && !self.cameraActive
            guard wMic != (self.prefLEDWatchMic && self.lastLEDMic) ||
                  wSpk != (self.prefLEDWatchSpk && self.lastLEDSpk) ||
                  wCam != (self.prefLEDCamColor != "none" && !self.lastLEDCam) ||
                  self.micMuted     != self.lastLEDMic ||
                  self.speakerMuted != self.lastLEDSpk ||
                  self.cameraActive != self.lastLEDCam else { return }
            self.lastLEDMic = self.micMuted
            self.lastLEDSpk = self.speakerMuted
            self.lastLEDCam = self.cameraActive
            if self.prefIconStyle == "ec" {
                self.statusItem.length = 28
            } else {
                let wm = self.prefLEDWatchMic && self.micMuted
                let ws = self.prefLEDWatchSpk && self.speakerMuted
                let watchedOne = (wm || ws) && !(wm && ws)
                self.statusItem.length = watchedOne ? 46 : 22
            }
            self.statusItem.button?.image = self.makeStatusImage()
            self.statusItem.button?.imageScaling = .scaleProportionallyDown
        }
    }

    // ── Mise à jour des items ─────────────────────────────────────────────────

    func updateMicItem(muted: Bool) {
        micMuted = muted
        DispatchQueue.main.async {
            self.micItem.title = muted ? "Mic : Muet" : "Mic : Actif"
            self.micItem.image = self.sfImage(muted ? "mic.slash.fill" : "mic.fill", muted: muted)
        }
        updateLED()
    }

    func updateSpeakerItem(muted: Bool, headphone: Bool = false) {
        speakerMuted = muted
        DispatchQueue.main.async {
            let label = headphone ? "Out" : "Speaker"
            self.speakerItem.title = muted ? "\(label) : Muet" : "\(label) : Actif"
            self.speakerItem.image = self.sfImage(muted ? "speaker.slash.fill" : "speaker.wave.2.fill", muted: muted)
        }
        updateLED()
    }

    func updateCameraItem(active: Bool) {
        cameraActive = active
        DispatchQueue.main.async {
            self.camItem.title = active ? "Caméra : Active" : "Caméra : Coupée"
            self.camItem.image = self.sfImage("camera.fill", muted: !active)
        }
        updateLED()
    }

    func updateRotItem(degree: Int) {
        DispatchQueue.main.async {
            self.rotItem.title = "Rotation : \(degree)°"
        }
    }

    func updateFanItems(cpuRPM: Int, gpuRPM: Int) {
        DispatchQueue.main.async {
            let cpuStr = cpuRPM > 0 ? "\(cpuRPM) RPM" : "Arrêté"
            let gpuStr = gpuRPM > 0 ? "\(gpuRPM) RPM" : "Arrêté"
            self.cpuFanItem.title = "CPU Fan : \(cpuStr)"
            self.gpuFanItem.title = "GPU Fan : \(gpuStr)"
        }
    }

    func updateCpuTemp(text: String) {
        DispatchQueue.main.async { self.cpuTempItem.title = text }
    }

    func updateSystemState(state: MSISystemState) {
        DispatchQueue.main.async {
            // Température
            let cpuStr = state.cpuTempC > 0 ? "\(state.cpuTempC) °C" : "—"
            self.cpuTempItem.title = "CPU : \(cpuStr)"

            // Fan mode
            let fm = FanMode(rawValue: state.fanMode) ?? .auto_
            if fm != self.currentFanMode {
                self.currentFanMode = fm
                self.updateFanModeItems(mode: fm)
            }

            // Cooler Boost
            let boost = state.coolerBoost != 0
            if boost != self.coolerBoostOn {
                self.coolerBoostOn = boost
                self.coolerBoostItem.title = "Cooler Boost : \(boost ? "ON 🔥" : "OFF")"
                self.coolerBoostItem.image = self.sfImage("flame", muted: boost)
            }
        }
    }

    func updateFanModeItems(mode: FanMode) {
        fanModeAutoItem.state   = (mode == .auto_)    ? .on : .off
        fanModeSilentItem.state = (mode == .silent)   ? .on : .off
        fanModeAdvItem.state    = (mode == .advanced) ? .on : .off
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    private func makeItem(title: String, sfSymbol: String, action: Selector?) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: "")
        item.target = self
        item.image  = sfImage(sfSymbol, muted: false)
        return item
    }

    // P2 : cache — évite de recréer l'image à chaque changement d'état
    private var sfImageCache: [String: NSImage] = [:]

    private func sfImage(_ name: String, muted: Bool) -> NSImage? {
        let key = "\(name)-\(muted)"
        if let cached = sfImageCache[key] { return cached }
        guard let img = NSImage(systemSymbolName: name, accessibilityDescription: nil) else { return nil }
        if muted {
    
            guard let tinted = img.copy() as? NSImage else { return img }
            tinted.lockFocus()
            NSColor.systemRed.withAlphaComponent(0.85).set()
            NSRect(origin: .zero, size: tinted.size).fill(using: .sourceAtop)
            tinted.unlockFocus()
            tinted.isTemplate = false
            sfImageCache[key] = tinted
            return tinted
        }
        img.isTemplate = true
        sfImageCache[key] = img
        return img
    }

    @objc private func tapMic()      { onToggleMic?() }
    @objc private func tapSpeaker()  { onToggleSpeaker?() }
    @objc private func tapCamera()   { onToggleCamera?() }
    @objc private func tapRotation() { onToggleRotation?() }
    @objc private func tapFanAuto()      { onSetFanMode?(.auto_) }
    @objc private func tapFanSilent()    { onSetFanMode?(.silent) }
    @objc private func tapFanAdvanced()  { onSetFanMode?(.advanced) }
    @objc private func tapBatteryLimit() { onToggleBatteryLimit?() }
    @objc private func tapFanCurve()     { onOpenFanCurve?() }


    @objc private func tapCoolerBoost()  { onToggleCoolerBoost?() }

    func updateBatteryLimitItem(percent: UInt8) {
        guard percent != currentBatteryLimit else { return }
        currentBatteryLimit = percent
        let sym = percent <= 80 ? "battery.50" : "battery.100"
        batteryLimitItem.title = "Charge : \(percent)%"
        batteryLimitItem.image = NSImage(systemSymbolName: sym, accessibilityDescription: nil)
    }

    // ── Backlight ──────────────────────────────────────────────────────────────

    func updateKbBacklightItems(level: UInt8) {
        currentKbBacklightLevel = level
        kbBacklightOffItem.state  = (level == 0) ? .on : .off
        kbBacklightLowItem.state  = (level == 1) ? .on : .off
        kbBacklightMedItem.state  = (level == 2) ? .on : .off
        kbBacklightHighItem.state = (level == 3) ? .on : .off
    }

    @objc private func tapKbOff()  { onSetKbBacklight?(0) }
    @objc private func tapKbLow()  { onSetKbBacklight?(1) }
    @objc private func tapKbMed()  { onSetKbBacklight?(2) }
    @objc private func tapKbHigh() { onSetKbBacklight?(3) }

    @objc private func tapECDump() { onRequestECDump?() }

    @objc private func tapPreferences() { onRequestPreferences?() }
    @objc private func tapQuit()     {
        NSLog("[MSIECToolboxAgent] Quitter depuis la barre de menu")
        NSApp.terminate(nil)
    }

    // ── Préférences — visibilité des sections ────────────────────────────────

    private func applyVisibilityPrefs() {
        setSection(audioItems,      sep: sepAfterAudio,     visible: prefShowAudio)
        setSection(monitoringItems, sep: sepAfterMonitor,   visible: prefShowMonitoring)
        setSection(fanItems,        sep: sepAfterFan,       visible: prefShowFan)
        setSection(batteryItems,    sep: sepAfterBattery,   visible: prefShowBattery)
        setSection(rotationItems,   sep: sepAfterRotation,  visible: prefShowRotation)
        setSection(backlightItems,  sep: sepAfterBacklight, visible: prefShowBacklight)
    }

    private func setSection(_ items: [NSMenuItem], sep: NSMenuItem?, visible: Bool) {
        items.forEach { $0.isHidden = !visible }
        sep?.isHidden = !visible
    }

    private func togglePref(_ key: String, current: inout Bool, items: [NSMenuItem],
                             sep: NSMenuItem?, menuTitle: String) {
        current.toggle()
        UserDefaults.standard.set(current, forKey: key)
        setSection(items, sep: sep, visible: current)
        // Mettre à jour la coche dans le sous-menu Préférences
        if let prefMenu = statusItem.menu?.items.first(where: { $0.submenu != nil })?.submenu {
            prefMenu.items.first(where: { $0.title == menuTitle })?.state = current ? .on : .off
        }
    }

    @objc func tapPrefAudio() {
        togglePref("pref_show_audio", current: &prefShowAudio,
                   items: audioItems, sep: sepAfterAudio, menuTitle: "Mic / Speaker / Caméra")
    }
    @objc func tapPrefMonitoring() {
        togglePref("pref_show_monitoring", current: &prefShowMonitoring,
                   items: monitoringItems, sep: sepAfterMonitor, menuTitle: "Surveillance (temp/RPM)")
    }
    @objc func tapPrefFan() {
        togglePref("pref_show_fan", current: &prefShowFan,
                   items: fanItems, sep: sepAfterFan, menuTitle: "Mode ventilation")
    }
    @objc func tapPrefBattery() {
        togglePref("pref_show_battery", current: &prefShowBattery,
                   items: batteryItems, sep: sepAfterBattery, menuTitle: "Limite de charge")
    }
    @objc func tapPrefRotation() {
        togglePref("pref_show_rotation", current: &prefShowRotation,
                   items: rotationItems, sep: sepAfterRotation, menuTitle: "Rotation écran")
    }
    @objc func tapPrefBacklight() {
        togglePref("pref_show_backlight", current: &prefShowBacklight,
                   items: backlightItems, sep: sepAfterBacklight, menuTitle: "Rétroéclairage clavier")
    }

    @objc func tapPrefLEDMic() {
        prefLEDWatchMic.toggle()
        UserDefaults.standard.set(prefLEDWatchMic, forKey: "pref_led_watch_mic")
        if let m = statusItem.menu?.items.first(where: { $0.submenu != nil })?.submenu {
            m.items.first(where: { $0.title == "Mic muet → orange/rouge" })?.state = prefLEDWatchMic ? .on : .off
        }
        lastLEDMic = !micMuted; updateLED()
    }

    @objc func tapPrefLEDSpk() {
        prefLEDWatchSpk.toggle()
        UserDefaults.standard.set(prefLEDWatchSpk, forKey: "pref_led_watch_spk")
        if let m = statusItem.menu?.items.first(where: { $0.submenu != nil })?.submenu {
            m.items.first(where: { $0.title == "Speaker muet → orange/rouge" })?.state = prefLEDWatchSpk ? .on : .off
        }
        lastLEDSpk = !speakerMuted; updateLED()
    }

    @objc func tapPrefLEDCamRouge() {
        prefLEDCamColor = "rouge"
        UserDefaults.standard.set("rouge", forKey: "pref_led_cam_color")
        lastLEDCam = !cameraActive; updateLED()
    }

    @objc func tapPrefLEDCamOrange() {
        prefLEDCamColor = "orange"
        UserDefaults.standard.set("orange", forKey: "pref_led_cam_color")
        lastLEDCam = !cameraActive; updateLED()
    }

    @objc func tapPrefLEDCamJaune() {
        prefLEDCamColor = "jaune"
        UserDefaults.standard.set("jaune", forKey: "pref_led_cam_color")
        lastLEDCam = !cameraActive; updateLED()
    }

    @objc func tapPrefLEDCamNone() {
        prefLEDCamColor = "none"
        UserDefaults.standard.set("none", forKey: "pref_led_cam_color")
        lastLEDCam = !cameraActive; updateLED()
    }

    @objc func tapPrefIconLED() {
        prefIconStyle = "led"
        UserDefaults.standard.set("led", forKey: "pref_icon_style")
        lastLEDMic = !micMuted  // forcer le redraw
        updateLED()
    }

    @objc func tapPrefIconEC() {
        prefIconStyle = "ec"
        UserDefaults.standard.set("ec", forKey: "pref_icon_style")
        statusItem.length = 28
        statusItem.button?.image = makeECBadgeImage()
        statusItem.button?.imageScaling = .scaleProportionallyDown
    }

}

// ---------------------------------------------------------------------------
// MARK: – Observateur principal
// ---------------------------------------------------------------------------

final class MuteObserver: NSObject, NSApplicationDelegate {

    private let client:  MSIECToolboxClient
    private let menuBar: MenuBarController

    // Deux paires de variables d'état séparées — intentionnel :
    //
    //                                 Utilisé par refresh() pour détecter un
    //                                 changement et éviter d'envoyer deux fois
    //                                 le même état au kext.
    //
    //   lastSentSpeaker / lastSentMic → dernier état effectivement envoyé au kext
    //                                 (setMuteState réussi). Utilisé par pollEC()
    //                                 pour comparer avec l'EC réel et éviter la
    //                                 boucle EC→CoreAudio→EC→… .
    //
    // Si setMuteState() échoue, lastSent* ne sont PAS mis à jour → le prochain
    // poll détectera la divergence et retentera la mise à jour.
    private var lastSentSpeaker = false
    private var lastSentMic     = false

    private var displayRotated   = false
    private var currentRotDegree = 0   // 0, 90, 180, 270 pour mode cycle
    private let kDisplayID       = "CC868235-2DF0-05C3-7FEE-80DFD78D701F"

    private var currentOutputID: AudioDeviceID = AudioDeviceID(kAudioObjectUnknown)
    private var currentInputID:  AudioDeviceID = AudioDeviceID(kAudioObjectUnknown)

    private var pollTimer:   DispatchSourceTimer?
    private var fanTimer:    DispatchSourceTimer?
    private var eventTap:    CFMachPort?
    private var eventTapSource: CFRunLoopSource?
    private var lastCamState = false  // mis à jour par pollEC() via EC 0x2E

    init(client: MSIECToolboxClient, menuBar: MenuBarController) {
        self.client  = client
        self.menuBar = menuBar
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        menuBar.setup()
        menuBar.onToggleMic      = { [weak self] in self?.toggleMicMute() }
        menuBar.onToggleSpeaker  = { [weak self] in self?.toggleSpeakerMute() }
        menuBar.onToggleCamera   = { [weak self] in self?.toggleCameraState() }
        menuBar.onToggleRotation = { [weak self] in self?.toggleDisplayRotation() }
        menuBar.onSetFanMode     = { [weak self] mode   in self?.setFanMode(mode) }
        menuBar.onSetKbBacklight    = { [weak self] level  in self?.setKbBacklight(level) }
        menuBar.onRequestECDump     = { [weak self] in self?.showECDump() }
        menuBar.onRequestPreferences = { [weak self] in self?.showPreferences() }
        menuBar.onToggleBatteryLimit = { [weak self] in self?.toggleBatteryLimit() }
        menuBar.onOpenFanCurve       = { [weak self] in self?.openFanCurvePanel() }
        menuBar.onToggleCoolerBoost = { [weak self] in self?.toggleCoolerBoost() }

        _ = client.connect()
        if let lvl = client.getKbBacklight() {
            DispatchQueue.main.async { self.menuBar.updateKbBacklightItems(level: lvl) }
        }
        if let pct = client.getBatteryCharge() {
            DispatchQueue.main.async { self.menuBar.updateBatteryLimitItem(percent: pct) }
        }
        client.watchService(
            onConnect:    { [weak self] in self?.refresh() },
            onDisconnect: { [weak self] in
                guard let self = self else { return }
                DispatchQueue.main.async {
                    self.menuBar.updateFanItems(cpuRPM: 0, gpuRPM: 0)
                    self.menuBar.updateCpuTemp(text: "CPU : kext non chargé")
                }
                NSLog("[MSIECToolboxAgent] ⚠️ kext déconnecté — LEDs et fan désactivés")
            })
        installDeviceChangeListeners()
        installMuteListeners(force: true)
        startECPolling()
        installKeyEventTap()

        NSLog("[MSIECToolboxAgent] Démarré v4.3.0 – barre de menu + CGEventTap + polling EC")
    }

    func applicationWillTerminate(_ notification: Notification) {
        // AUDIT-FIX-6 : supprimer le fichier PID à l'arrêt propre.
        // Évite qu'un PID recyclé par le kernel soit confondu avec notre agent.
        try? FileManager.default.removeItem(atPath: "/tmp/MSIECToolboxAgent.pid")
        pollTimer?.cancel()
        fanTimer?.cancel()
        accessibilityRetryTimer?.cancel()
        if let tap = eventTap { CGEvent.tapEnable(tap: tap, enable: false) }
        if let src = eventTapSource { CFRunLoopRemoveSource(CFRunLoopGetMain(), src, .commonModes); eventTapSource = nil }
        client.stopWatching()
        client.disconnect()
        NSLog("[MSIECToolboxAgent] Arrêté proprement")
    }

    // ── CGEventTap unique ────────────────────────────────────────────────────

    // Q4 : flag pour éviter d'envoyer la notification accessibilité deux fois
    private var accessibilityNotificationSent = false

    private func handleAccessibilityFailure() {
        if !accessibilityNotificationSent {
            accessibilityNotificationSent = true
            postAccessibilityNotification()
        }
        scheduleAccessibilityRetry()
    }

    private func installKeyEventTap() {
        let trusted = AXIsProcessTrustedWithOptions(
            [kAXTrustedCheckOptionPrompt.takeUnretainedValue(): false] as CFDictionary)

        if !trusted {
            NSLog("[MSIECToolboxAgent] ⚠️ Accessibilité non autorisée — tap clavier inactif")
            handleAccessibilityFailure()
            return
        }

        let ctx = Unmanaged.passUnretained(self).toOpaque()
        guard let tap = CGEvent.tapCreate(
            tap:              .cghidEventTap,
            place:            .headInsertEventTap,
            options:          .defaultTap,
            eventsOfInterest: CGEventMask(1 << CGEventType.keyDown.rawValue),
            callback: { _, type, event, userInfo -> Unmanaged<CGEvent>? in
                // macOS désactive le tap lors de Secure Input (dialogs auth)
                // On le réactive dès que possible
                if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
                    let me = Unmanaged<MuteObserver>.fromOpaque(userInfo!).takeUnretainedValue()
                    if let tap = me.eventTap { CGEvent.tapEnable(tap: tap, enable: true) }
                    return nil
                }
                guard type == .keyDown else { return Unmanaged.passRetained(event) }
                let keyCode = event.getIntegerValueField(.keyboardEventKeycode)
                let me = Unmanaged<MuteObserver>.fromOpaque(userInfo!).takeUnretainedValue()
                switch CGKeyCode(keyCode) {
                case kF14KeyCode:
                    NSLog("[MSIECToolboxAgent] F14 intercepté → toggle mic mute")
                    me.toggleMicMute()
                    return nil
                case kF13KeyCode:
                    NSLog("[MSIECToolboxAgent] F13 intercepté → toggle rotation écran")
                    me.toggleDisplayRotation()
                    return nil
                case kF6KeyCode:
                    NSLog("[MSIECToolboxAgent] F6 intercepté → toggle caméra")
                    me.toggleCameraState()
                    return nil
                case kF8KeyCode:
                    NSLog("[MSIECToolboxAgent] F8 intercepté → toggle rétroéclairage clavier")
                    me.toggleKbBacklight()
                    return nil
                default:
                    return Unmanaged.passRetained(event)
                }
            },
            userInfo: ctx)
        else {
            NSLog("[MSIECToolboxAgent] CGEventTap échoué — vérifier autorisation Accessibilité")
            handleAccessibilityFailure()
            return
        }
        let source = CFMachPortCreateRunLoopSource(nil, tap, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        CGEvent.tapEnable(tap: tap, enable: true)
        eventTap = tap
        eventTapSource = source
        NSLog("[MSIECToolboxAgent] CGEventTap installé — F5(79) + F6(118) + F8(100) + F12(111)")
    }

    // ── Notification Accessibilité ───────────────────────────────────────────

    private func postAccessibilityNotification() {
        // Log uniquement — pas d'ouverture automatique de Réglages Système.
        // Si le tap clavier est requis, accorder manuellement :
        // Réglages Système › Confidentialité › Accessibilité › MSIECToolboxAgent
        NSLog("[MSIECToolboxAgent] ⚠️  Accessibilité non accordée — tap clavier désactivé")
        NSLog("[MSIECToolboxAgent]    Pour activer les touches Fn : Réglages Système > Confidentialité > Accessibilité")
    }

    // Réessaie toutes les 10s jusqu'à ce que l'autorisation soit accordée
    private var accessibilityRetryTimer: DispatchSourceTimer?

    private func scheduleAccessibilityRetry() {
        let timer = DispatchSource.makeTimerSource(queue: .main)
        timer.schedule(deadline: .now() + 10, repeating: 10, leeway: .seconds(1))
        timer.setEventHandler { [weak self] in
            guard let self = self else { return }
            if AXIsProcessTrusted() {
                NSLog("[MSIECToolboxAgent] ✅ Accessibilité accordée — installation du tap")
                self.accessibilityRetryTimer?.cancel()
                self.accessibilityRetryTimer = nil
                self.installKeyEventTap()
            } else {
            }
        }
        timer.resume()
        accessibilityRetryTimer = timer
    }

    // ── Actions ──────────────────────────────────────────────────────────────

    private func toggleMicMute() {
        let deviceID = defaultInputDevice()
        guard deviceID != kAudioObjectUnknown else { return }
        let current = readMute(deviceID: deviceID, input: true)
        let newMuted = !current
        setAudioMute(deviceID: deviceID, input: true, muted: newMuted)
        if menuBar.prefShowOSD {
            DispatchQueue.main.async {
                MuteOSD.show(muted: newMuted, isMic: true)
            }
        }
    }

    private func toggleSpeakerMute() {
        let deviceID = defaultOutputDevice()
        guard deviceID != kAudioObjectUnknown else { return }
        let current = readMute(deviceID: deviceID, input: false)
        setAudioMute(deviceID: deviceID, input: false, muted: !current)
    }

    private func toggleDisplayRotation() {
        let newDegree: Int
        if menuBar.prefRotationMode == "90cycle" {
            currentRotDegree = (currentRotDegree + 90) % 360
            newDegree = currentRotDegree
            displayRotated = (newDegree != 0)
        } else {
            newDegree = displayRotated ? 0 : 180
            displayRotated = !displayRotated
            currentRotDegree = newDegree
        }
        menuBar.updateRotItem(degree: newDegree)

        NSLog("[MSIECToolboxAgent] Rotation écran → %d°", newDegree)
        let spec = "id:\(kDisplayID) res:1920x1080 color_depth:4 enabled:true scaling:off origin:(0,0) degree:\(newDegree)"
        // Exécution sur thread background pour ne pas geler la barre de menu
        DispatchQueue.global(qos: .userInitiated).async {
            let task = Process()
            task.executableURL = URL(fileURLWithPath: "/usr/local/bin/displayplacer")
            task.arguments = [spec]
            let pipe = Pipe()
            task.standardOutput = pipe
            task.standardError  = pipe
            try? task.run()
            task.waitUntilExit()
            let out = String(data: pipe.fileHandleForReading.readDataToEndOfFile(),
                             encoding: .utf8)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            if !out.isEmpty { NSLog("[MSIECToolboxAgent] displayplacer: %@", out) }
            NSLog("[MSIECToolboxAgent] Rotation terminée (exit=%d)", task.terminationStatus)
        }
    }

    // ── Toggle caméra — simule F6 pour piloter le firmware MSI ──────────────

    private func toggleCameraState() {
        let newState = !lastCamState  // lastCamState = cameraOff (true = coupée)
        if client.setCameraState(cameraOff: newState) {
            NSLog("[MSIECToolboxAgent] setCameraState → cameraOff=%d", newState ? 1 : 0)
            if menuBar.prefShowOSD {
                DispatchQueue.main.async {
                    MuteOSD.show(muted: newState, isMic: false, isCam: true)
                }
            }
        } else {
            NSLog("[MSIECToolboxAgent] setCameraState échoué — kext non connecté ?")
        }
    }

    // ── Fan mode, shift mode, cooler boost ───────────────────────────────────

    private func setFanMode(_ mode: FanMode) {
        if client.setFanMode(mode) {
            NSLog("[MSIECToolboxAgent] setFanMode → %@", mode.label)
            // pollEC() confirmera le mode dans 500ms via getSystemState
        }
    }

    private func setKbBacklight(_ level: UInt8) {
        if client.setKbBacklight(level: level) {
            NSLog("[MSIECToolboxAgent] setKbBacklight → %d", level)
            menuBar.updateKbBacklightItems(level: level)
        } else {
            NSLog("[MSIECToolboxAgent] setKbBacklight %d — kext non connecté", level)
        }
    }

    private func toggleKbBacklight() {
        let current = menuBar.currentKbBacklightLevel
        let next = (current + 1) % 4  // cycle 0→1→2→3→0
        setKbBacklight(next)
        NSLog("[MSIECToolboxAgent] toggleKbBacklight: %d → %d", current, next)
    }

    private func showPreferences() {
        DispatchQueue.main.async {
            PreferencesPanel.show(controller: self.menuBar)
        }
    }

    private func showECDump() {
        let fetcher: () -> [UInt8]? = { [weak self] in self?.client.dumpEC() }
        DispatchQueue.main.async {
            ECDumpPanel.show(fetcher: fetcher)
        }
    }

    private func toggleBatteryLimit() {
        let current = menuBar.currentBatteryLimit
        let next: UInt8 = (current <= 80) ? 100 : 80
        if client.setBatteryCharge(percent: next) {
            NSLog("[MSIECToolboxAgent] setBatteryCharge → %d%%", next)
            menuBar.updateBatteryLimitItem(percent: next)
        } else {
            NSLog("[MSIECToolboxAgent] setBatteryCharge — kext non connecté")
        }
    }

    private func openFanCurvePanel() {
        // Activer le mode Advanced si ce n'est pas déjà le cas
        if menuBar.currentFanMode != .advanced {
            _ = client.setFanMode(.advanced)
            menuBar.updateFanModeItems(mode: .advanced)
        }
        // Lire la courbe actuelle depuis l'EC
        let currentCurve = client.getFanCurve() ?? MSIFanCurve()
        DispatchQueue.main.async {
            FanCurvePanel.show(current: currentCurve) { [weak self] newCurve in
                guard let self = self else { return }
                if self.client.setFanCurve(newCurve) {
                    NSLog("[MSIECToolboxAgent] setFanCurve applied")

                } else {
                    NSLog("[MSIECToolboxAgent] setFanCurve failed — kext non connecté")
                }
            }
        }
    }

    private func toggleCoolerBoost() {
        let newState = !(menuBar.currentCoolerBoostOn)
        if client.setCoolerBoost(newState) {
            NSLog("[MSIECToolboxAgent] coolerBoost → %@", newState ? "ON" : "OFF")
        }
    }

    // ── Polling EC (500ms) — couvre mute, caméra, fan%, temp, modes ─────────
    // Le fanPolling séparé (2s) est conservé uniquement pour les RPM bruts.

    private func startECPolling() {
        // 500ms poll: system state (temps, fan %, modes) + LED sync
        let timer = DispatchSource.makeTimerSource(queue: .main)
        timer.schedule(deadline: .now() + 0.5, repeating: 0.5, leeway: .milliseconds(100))
        timer.setEventHandler { [weak self] in self?.pollEC() }
        timer.resume()
        pollTimer = timer

        // Dedicated 2s timer for RPM (4 EC reads via ISW formula).
        // Separated from the 500ms poll to reduce EC bus load:
        // RPM precision at 500ms is unnecessary for the menu bar display.
        let rpmTimer = DispatchSource.makeTimerSource(queue: .main)
        rpmTimer.schedule(deadline: .now() + 2.0, repeating: 2.0, leeway: .milliseconds(500))
        rpmTimer.setEventHandler { [weak self] in
            guard let self = self else { return }
            if let rpm = self.client.readFanRPM() {
                self.menuBar.updateFanItems(cpuRPM: rpm.cpuRPM, gpuRPM: rpm.gpuRPM)
            }
        }
        rpmTimer.resume()
        fanTimer = rpmTimer
    }

    private func pollEC() {
        // ── Mute / caméra via MSIAllState (sélecteur kMSIGetAllState) ──────
        if let mute = client.readECMuteState() {
            let speakerChanged = mute.speakerMuted != lastSentSpeaker
            let micChanged     = mute.micMuted     != lastSentMic
            let camChanged     = mute.cameraOff    != lastCamState

            if speakerChanged {
                setAudioMute(deviceID: defaultOutputDevice(), input: false, muted: mute.speakerMuted)
            }
            if micChanged {
                setAudioMute(deviceID: defaultInputDevice(), input: true, muted: mute.micMuted)
            }
            if camChanged {
                lastCamState = mute.cameraOff
                NSLog("[MSIECToolboxAgent] EC camera: %@", lastCamState ? "coupée" : "active")
                menuBar.updateCameraItem(active: !lastCamState)
            }
            if speakerChanged || micChanged {
                NSLog("[MSIECToolboxAgent] EC poll → speaker=%d mic=%d",
                      mute.speakerMuted ? 1 : 0, mute.micMuted ? 1 : 0)
                lastSentSpeaker = mute.speakerMuted
                lastSentMic     = mute.micMuted
            }
        }

        // ── Rétroéclairage clavier — sync si changé par firmware (Fn+F8) ──────
        if let lvl = client.getKbBacklight() {
            if lvl != menuBar.currentKbBacklightLevel {
                DispatchQueue.main.async { self.menuBar.updateKbBacklightItems(level: lvl) }
            }
        }

        // ── Fan / température / modes via MSISystemState (sélecteur kMSIGetSystemState) ──
        guard let sys = client.readSystemState() else { return }
        menuBar.updateSystemState(state: sys)

        // RPM updated by dedicated 2s timer (startECPolling)
    }

    // ── CoreAudio → EC ───────────────────────────────────────────────────────

    private func installDeviceChangeListeners() {
        var addrOut = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var addrIn = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultInputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        AudioObjectAddPropertyListenerBlock(
            AudioObjectID(kAudioObjectSystemObject), &addrOut, DispatchQueue.main) {
                [weak self] _, _ in self?.installMuteListeners(force: false); self?.refresh()
        }
        AudioObjectAddPropertyListenerBlock(
            AudioObjectID(kAudioObjectSystemObject), &addrIn, DispatchQueue.main) {
                [weak self] _, _ in self?.installMuteListeners(force: false); self?.refresh()
        }
    }

    // FIX LEAK : stocker les blocks listeners pour pouvoir les retirer
    // avant d'en ajouter de nouveaux quand le device change.
    private var outMuteListenerBlock: AudioObjectPropertyListenerBlock?
    private var inMuteListenerBlock:  AudioObjectPropertyListenerBlock?

    private func installMuteListeners(force: Bool) {
        let newOut = defaultOutputDevice()
        let newIn  = defaultInputDevice()
        if force || newOut != currentOutputID {
            // Retirer l'ancien listener
            if currentOutputID != kAudioObjectUnknown,
               let old = outMuteListenerBlock {
                var addr = muteAddress(input: false)
                AudioObjectRemovePropertyListenerBlock(currentOutputID, &addr,
                                                       DispatchQueue.main, old)
            }
            currentOutputID = newOut
            if newOut != kAudioObjectUnknown {
                let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in self?.refresh() }
                outMuteListenerBlock = block
                var addr = muteAddress(input: false)
                AudioObjectAddPropertyListenerBlock(newOut, &addr, DispatchQueue.main, block)
            }
        }
        if force || newIn != currentInputID {
            if currentInputID != kAudioObjectUnknown,
               let old = inMuteListenerBlock {
                var addr = muteAddress(input: true)
                AudioObjectRemovePropertyListenerBlock(currentInputID, &addr,
                                                       DispatchQueue.main, old)
            }
            currentInputID = newIn
            if newIn != kAudioObjectUnknown {
                let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in self?.refresh() }
                inMuteListenerBlock = block
                var addr = muteAddress(input: true)
                AudioObjectAddPropertyListenerBlock(newIn, &addr, DispatchQueue.main, block)
            }
        }
    }

    private func refresh() {
        let spk        = readMute(deviceID: defaultOutputDevice(), input: false)
        let mic        = readMute(deviceID: defaultInputDevice(),  input: true)
        let headphone  = isHeadphoneConnected()
        menuBar.headphoneConnected = headphone
        menuBar.updateMicItem(muted: mic)
        menuBar.updateSpeakerItem(muted: spk, headphone: headphone)
        guard spk != lastSentSpeaker || mic != lastSentMic else { return }
        NSLog("[MSIECToolboxAgent] CoreAudio → EC: speaker=%d mic=%d",
              spk ? 1 : 0, mic ? 1 : 0)
        if client.setMuteState(speaker: spk, mic: mic) {
            lastSentSpeaker = spk; lastSentMic = mic
        }
    }

    // ── Helpers CoreAudio ────────────────────────────────────────────────────

    private func setAudioMute(deviceID: AudioDeviceID, input: Bool, muted: Bool) {
        guard deviceID != kAudioObjectUnknown else { return }
        var value: UInt32 = muted ? 1 : 0
        var addr = muteAddress(input: input)
        AudioObjectSetPropertyData(deviceID, &addr, 0, nil,
                                   UInt32(MemoryLayout<UInt32>.size), &value)
    }

    private func muteAddress(input: Bool) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyMute,
            mScope:    input ? kAudioDevicePropertyScopeInput
                             : kAudioDevicePropertyScopeOutput,
            mElement:  kAudioObjectPropertyElementMain)
    }

    private func defaultOutputDevice() -> AudioDeviceID {
        var id = AudioDeviceID(kAudioObjectUnknown)
        var sz = UInt32(MemoryLayout<AudioDeviceID>.size)
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &sz, &id)
        return id
    }

    private func defaultInputDevice() -> AudioDeviceID {
        var id = AudioDeviceID(kAudioObjectUnknown)
        var sz = UInt32(MemoryLayout<AudioDeviceID>.size)
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultInputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &sz, &id)
        return id
    }

    // Retourne true si des écouteurs (ou tout périphérique de sortie externe)
    // sont branchés sur la prise jack. Détection via kAudioDevicePropertyDataSource :
    //   'hdpn' (0x6864706E) = Headphones
    //   'ispk' (0x6973706B) = Internal Speaker
    // Si la source courante est 'hdpn', on affiche "Out" au lieu de "Spk".
    private func isHeadphoneConnected() -> Bool {
        let deviceID = defaultOutputDevice()
        guard deviceID != kAudioObjectUnknown else { return false }
        var source: UInt32 = 0
        var sz = UInt32(MemoryLayout<UInt32>.size)
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyDataSource,
            mScope:    kAudioDevicePropertyScopeOutput,
            mElement:  kAudioObjectPropertyElementMain)
        let status = AudioObjectGetPropertyData(deviceID, &addr, 0, nil, &sz, &source)
        guard status == noErr else { return false }
        // 'hdpn' = 0x6864706E — source headphone jack
        return source == 0x6864706E
    }

    private func readMute(deviceID: AudioDeviceID, input: Bool) -> Bool {
        guard deviceID != kAudioObjectUnknown else { return false }
        var muted: UInt32 = 0
        var sz = UInt32(MemoryLayout<UInt32>.size)
        var addr = muteAddress(input: input)
        let kr = AudioObjectGetPropertyData(deviceID, &addr, 0, nil, &sz, &muted)
        return kr == noErr && muted != 0
    }
}

// ---------------------------------------------------------------------------
// MARK: – Point d'entrée NSApplication
// ---------------------------------------------------------------------------

// ── Guard instance unique ─────────────────────────────────────────────────
// Évite deux instances simultanées (double lancement manuel + launchd).
let pidFile = "/tmp/MSIECToolboxAgent.pid"
let myPID   = ProcessInfo.processInfo.processIdentifier

func isProcessRunning(_ pid: Int32) -> Bool {
    pid != myPID && kill(pid, 0) == 0
}

if let existing = try? String(contentsOfFile: pidFile, encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines),
   let existingPID = Int32(existing),
   isProcessRunning(existingPID) {
    NSLog("[MSIECToolboxAgent] ⚠️  Instance déjà en cours (PID %d) — arrêt", existingPID)
    exit(0)
}

try? String(myPID).write(toFile: pidFile, atomically: true, encoding: .utf8)

let app      = NSApplication.shared
let client   = MSIECToolboxClient()
let menuBar  = MenuBarController()
let observer = MuteObserver(client: client, menuBar: menuBar)

app.setActivationPolicy(.accessory)  // pas d'icône dans le Dock
app.delegate = observer
app.run()

// ---------------------------------------------------------------------------
// MARK: – FanCurvePanel
// NSPanel flottant — éditeur de courbe fan CPU (6 breakpoints temp + vitesse)
//
// Layout par ligne :
//   [Pt N]  [Temp: 50°C]  [————◉————]  [Spd: 0%]  [————◉————]
//
// Contraintes validées en temps réel :
//   - temp[i] strictement croissant (20–95°C)
//   - speed[i] croissant ou égal (0–100%)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// MARK: – Splash OSD mute mic
// ---------------------------------------------------------------------------

final class MuteOSD {

    private static var window: NSWindow?
    private static var hideTimer: Timer?

    static func show(muted: Bool, isMic: Bool, isCam: Bool = false) {
        hideTimer?.invalidate()

        // Créer la fenêtre si besoin
        if window == nil { window = makeWindow() }
        guard let win = window else { return }

        // Mettre à jour le contenu
        // contentView = NSVisualEffectView → OSDView est dans ses subviews
        let osdView = (win.contentView as? NSVisualEffectView)?.subviews.first as? OSDView
                   ?? win.contentView as? OSDView
        osdView?.configure(muted: muted, isMic: isMic, isCam: isCam)

        // Centrer légèrement en bas de l'écran (style macOS)
        if let screen = NSScreen.main {
            let sw = screen.frame.width, sh = screen.frame.height
            let ww = win.frame.width
            win.setFrameOrigin(NSPoint(
                x: screen.frame.minX + (sw - ww) / 2,
                y: screen.frame.minY + sh * 0.13
            ))
        }

        win.alphaValue = 0
        win.orderFrontRegardless()
        NSAnimationContext.runAnimationGroup { ctx in
            ctx.duration = 0.15
            win.animator().alphaValue = 1.0
        }

        // Disparaître après 1.8s
        hideTimer = Timer.scheduledTimer(withTimeInterval: 1.8, repeats: false) { _ in
            NSAnimationContext.runAnimationGroup({ ctx in
                ctx.duration = 0.3
                win.animator().alphaValue = 0
            }, completionHandler: {
                win.orderOut(nil)
            })
        }
    }

    private static func makeWindow() -> NSWindow {
        let size: CGFloat = 200
        let win = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: size, height: size),
            styleMask:   [.borderless],
            backing:     .buffered,
            defer:       false
        )
        win.isOpaque           = false
        win.backgroundColor    = .clear
        win.level              = .screenSaver
        win.ignoresMouseEvents = true
        win.collectionBehavior = [.canJoinAllSpaces, .stationary, .ignoresCycle]

        // NSVisualEffectView pour le fond flou — identique à l'OSD macOS
        let vfx = NSVisualEffectView(frame: NSRect(x: 0, y: 0, width: size, height: size))
        vfx.material    = .hudWindow   // gris neutre comme l'OSD volume/luminosité
        vfx.blendingMode = .behindWindow
        vfx.state       = .active
        vfx.wantsLayer  = true
        vfx.layer?.cornerRadius = 20
        vfx.layer?.masksToBounds = true

        let osdView = OSDView(frame: NSRect(x: 0, y: 0, width: size, height: size))
        osdView.wantsLayer = true
        osdView.layer?.backgroundColor = NSColor.clear.cgColor
        vfx.addSubview(osdView)
        win.contentView = vfx

        return win
    }
}

// Vue OSD — fond flou + icône + texte
private final class OSDView: NSView {

    private var muted:  Bool = true
    private var isMic:  Bool = true
    private var isCam:  Bool = false

    func configure(muted: Bool, isMic: Bool, isCam: Bool = false) {
        self.muted = muted
        self.isMic = isMic
        self.isCam = isCam
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        let b = bounds
        let size = b.width

        // Le fond est géré par NSVisualEffectView — on dessine seulement l'icône et le texte

        // ── SF Symbol principal ────────────────────────────────────────────
        let symName: String
        if isCam {
            symName = "camera.fill"  // barre dessinée manuellement si coupée
        } else if isMic {
            symName = muted ? "mic.slash.fill" : "mic.fill"
        } else {
            symName = muted ? "speaker.slash.fill" : "speaker.wave.2.fill"
        }

        // Dessiner l'icône SF Symbol en respectant son ratio naturel
        if let sym = NSImage(systemSymbolName: symName, accessibilityDescription: nil) {
            let cfg = NSImage.SymbolConfiguration(pointSize: 52, weight: .medium)
                .applying(NSImage.SymbolConfiguration(paletteColors: [.labelColor]))
            let img = sym.withSymbolConfiguration(cfg) ?? sym
            // Utiliser la taille naturelle de l'image (pas de déformation)
            let iw = img.size.width
            let ih = img.size.height
            let iconX = (size - iw) / 2
            let iconY = (size - ih) / 2 + 14
            img.draw(in: NSRect(x: iconX, y: iconY, width: iw, height: ih),
                     from: .zero, operation: .sourceOver, fraction: 1.0,
                     respectFlipped: true, hints: nil)
        }

        // Barre diagonale rouge pour caméra coupée
        if isCam && muted, let sym2 = NSImage(systemSymbolName: symName, accessibilityDescription: nil) {
            let cfg2 = NSImage.SymbolConfiguration(pointSize: 52, weight: .medium)
            let img2 = sym2.withSymbolConfiguration(cfg2) ?? sym2
            let iw2 = img2.size.width, ih2 = img2.size.height
            let ix2 = (size - iw2) / 2, iy2 = (size - ih2) / 2 + 14
            let x1 = ix2 + iw2 * 0.10, y1 = iy2 + ih2 * 0.90
            let x2 = ix2 + iw2 * 0.90, y2 = iy2 + ih2 * 0.10
            let slash = NSBezierPath()
            slash.move(to:  CGPoint(x: x1, y: y1))
            slash.line(to:  CGPoint(x: x2, y: y2))
            slash.lineWidth  = 7
            slash.lineCapStyle = .round
            // Bordure blanche pour lisibilité
            NSColor.white.withAlphaComponent(0.6).setStroke()
            slash.lineWidth = 9
            slash.stroke()
            NSColor(red: 1.0, green: 0.2, blue: 0.2, alpha: 1.0).setStroke()
            slash.lineWidth = 6
            slash.stroke()
        }

        // ── Texte en bas ──────────────────────────────────────────────────
        let label: String
        if isCam {
            label = muted ? "Caméra coupée" : "Caméra active"
        } else if isMic {
            label = muted ? "Mic muet" : "Mic actif"
        } else {
            label = muted ? "Coupé" : "Actif"
        }
        let attrs: [NSAttributedString.Key: Any] = [
            .font:            NSFont.systemFont(ofSize: 13, weight: .semibold),
            .foregroundColor: NSColor.labelColor
        ]
        let str = NSAttributedString(string: label, attributes: attrs)
        let sw = str.size().width
        str.draw(at: CGPoint(x: (size - sw) / 2, y: 16))
    }
}

// ---------------------------------------------------------------------------
// MARK: – Fenêtre Préférences
// ---------------------------------------------------------------------------

final class PreferencesPanel: NSObject {

    // Lazily created on first open, freed on close (see NSWindowDelegate below)
    static var shared: PreferencesPanel?
    private var panel: NSPanel!
    private weak var controller: MenuBarController?

    static func show(controller: MenuBarController) {
        if shared == nil { shared = PreferencesPanel() }
        shared!.controller = controller
        shared!.reload()
        shared!.panel.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    private override init() {
        super.init()
        buildPanel()
    }

    // Références aux checkboxes pour reload()
    private var cbAudio:      NSButton!
    private var cbOSD:        NSButton!
    private var cbMonitor:    NSButton!
    private var cbFan:        NSButton!
    private var cbBattery:    NSButton!
    private var cbRotation:   NSButton!
    private var cbBacklight:  NSButton!
    private var cbLEDMic:     NSButton!
    private var cbLEDSpk:     NSButton!
    private var rbCamRouge:   NSButton!
    private var rbCamOrange:  NSButton!
    private var rbCamJaune:   NSButton!
    private var rbCamNone:    NSButton!
    private var rbLED:        NSButton!
    private var rbEC:         NSButton!
    private var rbRot180:     NSButton!
    private var rbRotCycle:   NSButton!

    private func buildPanel() {
        let W: CGFloat = 320
        let H: CGFloat = 560

        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: W, height: H),
            styleMask:   [.titled, .closable, .nonactivatingPanel],
            backing:     .buffered,
            defer:       false
        )
        panel.title = "Préférences — MSIECToolbox"
        panel.level = .floating
        panel.isReleasedWhenClosed = false
        panel.center()

        let root = NSView(frame: NSRect(x: 0, y: 0, width: W, height: H))
        panel.contentView = root

        var y = H - 20

        func sectionLabel(_ text: String) {
            let f = NSTextField(frame: NSRect(x: 16, y: y - 18, width: W - 32, height: 16))
            f.stringValue = text
            f.isEditable = false; f.isBezeled = false; f.drawsBackground = false
            f.font = NSFont.boldSystemFont(ofSize: 11)
            f.textColor = .secondaryLabelColor
            root.addSubview(f)
            y -= 22
        }

        func checkbox(_ title: String, action: Selector) -> NSButton {
            let b = NSButton(checkboxWithTitle: title, target: self, action: action)
            b.frame = NSRect(x: 20, y: y - 20, width: W - 40, height: 20)
            b.font = NSFont.systemFont(ofSize: 12)
            root.addSubview(b)
            y -= 24
            return b
        }

        func separator() {
            let line = NSBox(frame: NSRect(x: 16, y: y - 4, width: W - 32, height: 1))
            line.boxType = .separator
            root.addSubview(line)
            y -= 12
        }

        func radioButton(_ title: String, action: Selector) -> NSButton {
            let b = NSButton(radioButtonWithTitle: title, target: self, action: action)
            b.frame = NSRect(x: 20, y: y - 20, width: W - 40, height: 20)
            b.font = NSFont.systemFont(ofSize: 12)
            root.addSubview(b)
            y -= 24
            return b
        }

        // ── Sections visibles ─────────────────────────────────────────────
        sectionLabel("Sections du menu")
        cbAudio     = checkbox("Mic / Speaker / Caméra",    action: #selector(tapCbAudio))
        cbMonitor   = checkbox("Surveillance (temp/RPM)",   action: #selector(tapCbMonitor))
        cbFan       = checkbox("Mode ventilation",          action: #selector(tapCbFan))
        cbBattery   = checkbox("Limite de charge",          action: #selector(tapCbBattery))
        cbRotation  = checkbox("Rotation écran",            action: #selector(tapCbRotation))
        cbBacklight = checkbox("Rétroéclairage clavier",    action: #selector(tapCbBacklight))

        separator()

        // ── Couleur LED ───────────────────────────────────────────────────
        sectionLabel("Couleur LED — surveiller")
        cbLEDMic = checkbox("Mic muet → orange / rouge",    action: #selector(tapCbLEDMic))
        cbLEDSpk = checkbox("Speaker muet → orange / rouge", action: #selector(tapCbLEDSpk))
        let camHeader = NSTextField(frame: NSRect(x: 20, y: y - 18, width: W - 40, height: 16))
        camHeader.stringValue = "Caméra coupée →"
        camHeader.isEditable = false; camHeader.isBezeled = false; camHeader.drawsBackground = false
        camHeader.font = NSFont.systemFont(ofSize: 12)
        root.addSubview(camHeader)
        y -= 22

        rbCamRouge  = radioButton("Rouge",              action: #selector(tapRbCamRouge))
        rbCamOrange = radioButton("Orange",             action: #selector(tapRbCamOrange))
        rbCamJaune  = radioButton("Jaune + label \"cam\"", action: #selector(tapRbCamJaune))
        rbCamNone   = radioButton("Non concerné",       action: #selector(tapRbCamNone))

        separator()

        // ── Icône barre de menus ──────────────────────────────────────────
        sectionLabel("Icône barre de menus")
        rbLED = radioButton("LEDs dynamiques", action: #selector(tapRbLED))
        rbEC  = radioButton("Badge EC",         action: #selector(tapRbEC))

        separator()

        // ── Mode rotation ────────────────────────────────────────────────
        sectionLabel("Rotation écran (F12)")
        rbRot180  = radioButton("Bascule 0° / 180°",         action: #selector(tapRb180))
        rbRotCycle = radioButton("Cycle 0° → 90° → 180° → 270°", action: #selector(tapRbCycle))

        separator()

        // ── OSD ──────────────────────────────────────────────────────────
        sectionLabel("Notifications visuelles")
        cbOSD = checkbox("Afficher l'OSD (mic / caméra)", action: #selector(tapCbOSD))

        // Footer
        let note = NSTextField(frame: NSRect(x: 16, y: 12, width: W - 32, height: 16))
        note.stringValue = "Les préférences sont sauvegardées automatiquement"
        note.isEditable = false; note.isBezeled = false; note.drawsBackground = false
        note.font = NSFont.systemFont(ofSize: 9); note.textColor = .tertiaryLabelColor
        root.addSubview(note)
    }

    func reload() {
        guard let c = controller else { return }
        cbAudio.state     = c.prefShowAudio      ? .on : .off
        cbMonitor.state   = c.prefShowMonitoring  ? .on : .off
        cbFan.state       = c.prefShowFan         ? .on : .off
        cbBattery.state   = c.prefShowBattery     ? .on : .off
        cbRotation.state  = c.prefShowRotation    ? .on : .off
        cbBacklight.state = c.prefShowBacklight   ? .on : .off
        cbLEDMic.state    = c.prefLEDWatchMic     ? .on : .off
        cbLEDSpk.state    = c.prefLEDWatchSpk     ? .on : .off
        rbCamRouge.state  = (c.prefLEDCamColor == "rouge")  ? .on : .off
        rbCamOrange.state = (c.prefLEDCamColor == "orange") ? .on : .off
        rbCamJaune.state  = (c.prefLEDCamColor == "jaune")  ? .on : .off
        rbCamNone.state   = (c.prefLEDCamColor == "none")   ? .on : .off
        rbLED.state       = (c.prefIconStyle == "led") ? .on : .off
        rbEC.state        = (c.prefIconStyle == "ec")  ? .on : .off
        rbRot180.state    = (c.prefRotationMode == "180")     ? .on : .off
        rbRotCycle.state  = (c.prefRotationMode == "90cycle") ? .on : .off
        cbOSD.state       = c.prefShowOSD ? .on : .off
    }

    @objc private func tapCbAudio()     { controller?.tapPrefAudio() }
    @objc private func tapCbMonitor()   { controller?.tapPrefMonitoring() }
    @objc private func tapCbFan()       { controller?.tapPrefFan() }
    @objc private func tapCbBattery()   { controller?.tapPrefBattery() }
    @objc private func tapCbRotation()  { controller?.tapPrefRotation() }
    @objc private func tapCbBacklight() { controller?.tapPrefBacklight() }
    @objc private func tapCbLEDMic()    { controller?.tapPrefLEDMic();     reload() }
    @objc private func tapCbLEDSpk()    { controller?.tapPrefLEDSpk();     reload() }
    @objc private func tapRbCamRouge()  { controller?.tapPrefLEDCamRouge();  reload() }
    @objc private func tapRbCamOrange() { controller?.tapPrefLEDCamOrange(); reload() }
    @objc private func tapRbCamJaune()  { controller?.tapPrefLEDCamJaune();  reload() }
    @objc private func tapRbCamNone()   { controller?.tapPrefLEDCamNone();   reload() }
    @objc private func tapRbLED()       { controller?.tapPrefIconLED();     reload() }
    @objc private func tapRbEC()        { controller?.tapPrefIconEC();      reload() }
    @objc private func tapCbOSD() {
        controller?.prefShowOSD.toggle()
        UserDefaults.standard.set(controller?.prefShowOSD ?? true, forKey: "pref_show_osd")
        reload()
    }
    @objc private func tapRb180()       {
        controller?.prefRotationMode = "180"
        UserDefaults.standard.set("180", forKey: "pref_rotation_mode")
        reload()
    }
    @objc private func tapRbCycle()     {
        controller?.prefRotationMode = "90cycle"
        UserDefaults.standard.set("90cycle", forKey: "pref_rotation_mode")
        reload()
    }
}

// ---------------------------------------------------------------------------
// MARK: – Fenêtre Table EC
// ---------------------------------------------------------------------------

final class ECDumpPanel: NSObject {

    static var shared: ECDumpPanel?

    private var panel:       NSPanel!
    private var cells:       [[NSTextField]] = []  // 16 lignes × 16 colonnes
    private var rowLabels:   [NSTextField] = []
    private var fetcher:     (() -> [UInt8]?)?
    private var autoRefreshTimer: Timer?
    private var autoRefreshCb: NSButton!

    static func show(fetcher: @escaping () -> [UInt8]?) {
        if shared == nil { shared = ECDumpPanel() }
        shared!.fetcher = fetcher
        shared!.refresh()
        shared!.panel.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    static func hide() {
        stop()
        shared?.panel.orderOut(nil)
        shared = nil  // free NSView tree (~0.4MB) when dismissed
    }

    private override init() {
        super.init()
        buildPanel()
    }

    private func buildPanel() {
        let colW:    CGFloat = 30
        let rowH:    CGFloat = 18
        let labelW:  CGFloat = 36
        let marginL: CGFloat = 12
        let marginB: CGFloat = 50
        let headerH: CGFloat = 24
        let cols = 16
        let rows = 16
        let W = marginL + labelW + CGFloat(cols) * colW + 12
        let H = marginB + headerH + CGFloat(rows) * rowH + 12

        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: W, height: H),
            styleMask:   [.titled, .closable, .nonactivatingPanel],
            backing:     .buffered,
            defer:       false
        )
        panel.title = "Table EC — MSI Modern 15 A10M"
        panel.level = .floating
        panel.isReleasedWhenClosed = false
        panel.center()

        let root = NSView(frame: NSRect(x: 0, y: 0, width: W, height: H))
        panel.contentView = root

        func makeLabel(_ text: String, x: CGFloat, y: CGFloat,
                        w: CGFloat, h: CGFloat = 16,
                        mono: Bool = false, bold: Bool = false,
                        align: NSTextAlignment = .center) -> NSTextField {
            let f = NSTextField(frame: NSRect(x: x, y: y, width: w, height: h))
            f.stringValue = text
            f.isEditable = false; f.isBezeled = false; f.drawsBackground = false
            f.alignment = align
            if bold        { f.font = NSFont.boldSystemFont(ofSize: 10) }
            else if mono   { f.font = NSFont.monospacedSystemFont(ofSize: 10, weight: .regular) }
            else           { f.font = NSFont.systemFont(ofSize: 10) }
            return f
        }

        // En-tête colonnes (_0 … _F)
        let hY = H - headerH
        root.addSubview(makeLabel("", x: marginL, y: hY, w: labelW, bold: true))
        for col in 0..<cols {
            let x = marginL + labelW + CGFloat(col) * colW
            root.addSubview(makeLabel(String(format: "_%X", col),
                                       x: x, y: hY, w: colW, bold: true))
        }

        // Lignes
        for row in 0..<rows {
            let y = H - headerH - CGFloat(row + 1) * rowH - 2

            // Label ligne (0x0_ … 0xF_)
            let rl = makeLabel(String(format: "0x%X_", row),
                                x: marginL, y: y, w: labelW, mono: true, bold: true, align: .left)
            root.addSubview(rl)
            rowLabels.append(rl)

            var rowCells: [NSTextField] = []
            for col in 0..<cols {
                let x = marginL + labelW + CGFloat(col) * colW
                let cell = makeLabel("00", x: x, y: y, w: colW, mono: true)
                root.addSubview(cell)
                rowCells.append(cell)
            }
            cells.append(rowCells)
        }

        // Footer
        let refreshBtn = NSButton(frame: NSRect(x: W - 110, y: 12, width: 95, height: 26))
        refreshBtn.title = "Rafraîchir"
        refreshBtn.bezelStyle = .rounded
        refreshBtn.keyEquivalent = "\r"
        refreshBtn.target = self
        refreshBtn.action = #selector(tapRefresh)
        root.addSubview(refreshBtn)

        autoRefreshCb = NSButton(checkboxWithTitle: "Auto (2s)", target: self,
                                  action: #selector(tapAutoRefresh))
        autoRefreshCb.frame = NSRect(x: W - 220, y: 14, width: 100, height: 20)
        autoRefreshCb.font = NSFont.systemFont(ofSize: 11)
        root.addSubview(autoRefreshCb)

        let note = NSTextField(frame: NSRect(x: marginL, y: 15, width: 180, height: 18))
        note.stringValue = "256 registres EC — kext direct"
        note.isEditable = false; note.isBezeled = false; note.drawsBackground = false
        note.font = NSFont.systemFont(ofSize: 9); note.textColor = .secondaryLabelColor
        root.addSubview(note)
    }

    func refresh() {
        guard let bytes = fetcher?() else { return }
        DispatchQueue.main.async {
            for row in 0..<16 {
                for col in 0..<16 {
                    let val = bytes[row * 16 + col]
                    let cell = self.cells[row][col]
                    cell.stringValue = String(format: "%02X", val)
                    // Mettre en évidence les valeurs non-nulles
                    cell.textColor = (val == 0) ? .tertiaryLabelColor : .labelColor
                }
            }
        }
    }

    @objc private func tapRefresh() { refresh() }

    @objc private func tapAutoRefresh() {
        if autoRefreshCb.state == .on {
            autoRefreshTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
                self?.refresh()
            }
        } else {
            autoRefreshTimer?.invalidate()
            autoRefreshTimer = nil
        }
    }

    // Arrêter le timer quand le panel se ferme
    static func stop() {
        shared?.autoRefreshTimer?.invalidate()
        shared?.autoRefreshTimer = nil
        shared?.autoRefreshCb?.state = .off
    }
}

final class FanCurvePanel: NSObject {

    static var shared: FanCurvePanel?

    private var panel:    NSPanel!
    private var tempSliders:  [NSSlider] = []
    private var speedSliders: [NSSlider] = []
    private var tempLabels:   [NSTextField] = []
    private var speedLabels:  [NSTextField] = []
    private var applyBtn:    NSButton!
    private var resetBtn:    NSButton!
    private var profilePopup: NSPopUpButton!
    private var saveBtn:     NSButton!
    private var deleteBtn:   NSButton!
    private var onApply:   ((MSIFanCurve) -> Void)?
    private var curve:     MSIFanCurve

    static func show(current: MSIFanCurve, onApply: @escaping (MSIFanCurve) -> Void) {
        if shared == nil { shared = FanCurvePanel() }
        shared!.configure(curve: current, onApply: onApply)
        shared!.panel.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    private override init() {
        curve = MSIFanCurve()
        super.init()
        buildPanel()
    }

    private func buildPanel() {
        let W: CGFloat = 680
        let rowH: CGFloat = 36
        let headerH: CGFloat = 30
        let footerH: CGFloat = 80
        let H = headerH + CGFloat(6) * rowH + footerH + 20

        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: W, height: H),
            styleMask:   [.titled, .closable, .nonactivatingPanel],
            backing:     .buffered,
            defer:       false
        )
        panel.title        = "Courbe fan CPU — Mode Avancé"
        panel.level        = .floating
        panel.isReleasedWhenClosed = false
        panel.center()

        let root = NSView(frame: NSRect(x: 0, y: 0, width: W, height: H))
        panel.contentView = root

        // ── En-tête ──────────────────────────────────────────────────────────
        func label(_ t: String, x: CGFloat, y: CGFloat, w: CGFloat, bold: Bool = false) -> NSTextField {
            let f = NSTextField(frame: NSRect(x: x, y: y, width: w, height: 18))
            f.stringValue = t
            f.isEditable = false; f.isBezeled = false; f.drawsBackground = false
            f.alignment = .center
            if bold { f.font = NSFont.boldSystemFont(ofSize: 11) }
            else     { f.font = NSFont.systemFont(ofSize: 11) }
            return f
        }
        let hY = H - headerH
        root.addSubview(label("Point", x: 10,  y: hY, w: 40,  bold: true))
        root.addSubview(label("Température (°C)", x: 55, y: hY, w: 185, bold: true))
        root.addSubview(label("Vitesse fan (%)", x: 270, y: hY, w: 185, bold: true))
        root.addSubview(label("Défaut", x: 465, y: hY, w: 50,  bold: true))

        let defaultCurve = MSIFanCurve()

        for i in 0..<6 {
            let y = H - headerH - CGFloat(i + 1) * rowH

            // Numéro du point
            root.addSubview(label("\(i+1)", x: 10, y: y + 9, w: 40))

            // Température slider
            let tSlider = NSSlider(frame: NSRect(x: 55, y: y + 6, width: 130, height: 22))
            tSlider.minValue = 20; tSlider.maxValue = 95
            tSlider.integerValue = Int(defaultCurve.tempsArray[i])
            tSlider.tag = i
            tSlider.target = self; tSlider.action = #selector(tempChanged(_:))
            root.addSubview(tSlider)
            tempSliders.append(tSlider)

            let tLabel = NSTextField(frame: NSRect(x: 190, y: y + 9, width: 50, height: 18))
            tLabel.isEditable = false; tLabel.isBezeled = false; tLabel.drawsBackground = false
            tLabel.alignment = .left
            tLabel.font = NSFont.monospacedDigitSystemFont(ofSize: 11, weight: .regular)
            tLabel.stringValue = "\(defaultCurve.tempsArray[i]) °C"
            root.addSubview(tLabel)
            tempLabels.append(tLabel)

            // Vitesse slider
            let sSlider = NSSlider(frame: NSRect(x: 270, y: y + 6, width: 130, height: 22))
            sSlider.minValue = 0; sSlider.maxValue = 100
            sSlider.integerValue = Int(defaultCurve.speedsArray[i])
            sSlider.tag = i
            sSlider.target = self; sSlider.action = #selector(speedChanged(_:))
            root.addSubview(sSlider)
            speedSliders.append(sSlider)

            let sLabel = NSTextField(frame: NSRect(x: 405, y: y + 9, width: 50, height: 18))
            sLabel.isEditable = false; sLabel.isBezeled = false; sLabel.drawsBackground = false
            sLabel.alignment = .left
            sLabel.font = NSFont.monospacedDigitSystemFont(ofSize: 11, weight: .regular)
            sLabel.stringValue = "\(defaultCurve.speedsArray[i]) %"
            root.addSubview(sLabel)
            speedLabels.append(sLabel)

            // Valeur défaut firmware
            root.addSubview(label("\(defaultCurve.tempsArray[i])/\(defaultCurve.speedsArray[i])",
                                   x: 460, y: y + 9, w: 55))
        }

        // ── Footer — ligne 1 : profils ──────────────────────────────────────
        let profileLabel = NSTextField(frame: NSRect(x: 10, y: 52, width: 60, height: 18))
        profileLabel.stringValue = "Profil :"
        profileLabel.isEditable = false; profileLabel.isBezeled = false; profileLabel.drawsBackground = false
        profileLabel.font = NSFont.systemFont(ofSize: 11)
        root.addSubview(profileLabel)

        profilePopup = NSPopUpButton(frame: NSRect(x: 72, y: 48, width: 200, height: 26))
        profilePopup.font = NSFont.systemFont(ofSize: 11)
        profilePopup.target = self; profilePopup.action = #selector(tapLoadProfile)
        root.addSubview(profilePopup)

        saveBtn = NSButton(frame: NSRect(x: 280, y: 48, width: 100, height: 26))
        saveBtn.title = "Sauvegarder..."
        saveBtn.bezelStyle = .rounded
        saveBtn.target = self; saveBtn.action = #selector(tapSaveProfile)
        root.addSubview(saveBtn)

        deleteBtn = NSButton(frame: NSRect(x: 386, y: 48, width: 80, height: 26))
        deleteBtn.title = "Supprimer"
        deleteBtn.bezelStyle = .rounded
        deleteBtn.target = self; deleteBtn.action = #selector(tapDeleteProfile)
        root.addSubview(deleteBtn)

        // ── Footer — ligne 2 : apply / reset ─────────────────────────────────
        applyBtn = NSButton(frame: NSRect(x: W - 110, y: 12, width: 95, height: 28))
        applyBtn.title = "Appliquer"
        applyBtn.bezelStyle = .rounded
        applyBtn.keyEquivalent = "\r"
        applyBtn.target = self; applyBtn.action = #selector(tapApply)
        root.addSubview(applyBtn)

        resetBtn = NSButton(frame: NSRect(x: W - 220, y: 12, width: 100, height: 28))
        resetBtn.title = "Défaut firmware"
        resetBtn.bezelStyle = .rounded
        resetBtn.target = self; resetBtn.action = #selector(tapReset)
        root.addSubview(resetBtn)

        let note = NSTextField(frame: NSRect(x: 10, y: 15, width: 290, height: 24))
        note.stringValue = "Point 7 : 100% fixe (non éditable) — mode Advanced requis"
        note.isEditable = false; note.isBezeled = false; note.drawsBackground = false
        note.font = NSFont.systemFont(ofSize: 9); note.textColor = .secondaryLabelColor
        root.addSubview(note)
    }

    private func configure(curve: MSIFanCurve, onApply: @escaping (MSIFanCurve) -> Void) {
        self.onApply = onApply
        self.curve   = curve
        let t = curve.tempsArray
        let s = curve.speedsArray
        for i in 0..<6 {
            tempSliders[i].integerValue  = Int(t[i])
            speedSliders[i].integerValue = Int(s[i])
            tempLabels[i].stringValue    = "\(t[i]) °C"
            speedLabels[i].stringValue   = "\(s[i]) %"
        }
        reloadProfilePopup()
    }

    func reloadProfilePopup() {
        profilePopup.removeAllItems()
        profilePopup.addItem(withTitle: "— Choisir un profil —")
        FanProfileManager.shared.profiles.forEach { profilePopup.addItem(withTitle: $0.name) }
        deleteBtn.isEnabled = FanProfileManager.shared.profiles.count > 0
    }

    @objc private func tapLoadProfile() {
        let name = profilePopup.titleOfSelectedItem ?? ""
        guard let profile = FanProfileManager.shared.profile(named: name) else { return }
        configure(curve: profile.toCurve(), onApply: onApply ?? { _ in })
    }

    @objc private func tapSaveProfile() {
        let alert = NSAlert()
        alert.messageText = "Sauvegarder le profil"
        alert.informativeText = "Entrez un nom pour ce profil :"
        alert.addButton(withTitle: "Sauvegarder")
        alert.addButton(withTitle: "Annuler")
        let tf = NSTextField(frame: NSRect(x: 0, y: 0, width: 240, height: 24))
        tf.placeholderString = "Ex : Silencieux, Jeu, Turbo..."
        alert.accessoryView = tf
        alert.window.initialFirstResponder = tf
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let name = tf.stringValue.trimmingCharacters(in: .whitespaces)
        guard !name.isEmpty else { return }
        let profile = FanProfile.fromCurve(readCurve(), name: name)
        FanProfileManager.shared.add(profile)
        reloadProfilePopup()
        // Sélectionner le profil sauvegardé
        profilePopup.selectItem(withTitle: name)
        NSLog("[MSIECToolboxAgent] Profil sauvegardé : \(name)")
    }

    @objc private func tapDeleteProfile() {
        let name = profilePopup.titleOfSelectedItem ?? ""
        guard FanProfileManager.shared.profile(named: name) != nil else { return }
        let alert = NSAlert()
        alert.messageText = "Supprimer le profil \"\(name)\""
        alert.informativeText = "Cette action est irréversible."
        alert.addButton(withTitle: "Supprimer")
        alert.addButton(withTitle: "Annuler")
        alert.buttons[0].hasDestructiveAction = true
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        FanProfileManager.shared.delete(name: name)
        reloadProfilePopup()
        NSLog("[MSIECToolboxAgent] Profil supprimé : \(name)")
    }

    private func readCurve() -> MSIFanCurve {
        let t = tempSliders.map  { UInt8($0.integerValue) }
        let s = speedSliders.map { UInt8($0.integerValue) }
        return MSIFanCurve(
            temps:  (t[0],t[1],t[2],t[3],t[4],t[5]),
            speeds: (s[0],s[1],s[2],s[3],s[4],s[5])
        )
    }

    @objc private func tempChanged(_ sender: NSSlider) {
        let i = sender.tag
        var val = sender.integerValue
        // Contrainte : temp[i] > temp[i-1]
        if i > 0 { val = max(val, tempSliders[i-1].integerValue + 1) }
        // Contrainte : temp[i] < temp[i+1]
        if i < 5 { val = min(val, tempSliders[i+1].integerValue - 1) }
        sender.integerValue = val
        tempLabels[i].stringValue = "\(val) °C"
    }

    @objc private func speedChanged(_ sender: NSSlider) {
        let i = sender.tag
        var val = sender.integerValue
        if i > 0 { val = max(val, speedSliders[i-1].integerValue) }
        if i < 5 { val = min(val, speedSliders[i+1].integerValue) }
        sender.integerValue = val
        speedLabels[i].stringValue = "\(val) %"
    }

    @objc private func tapApply() {
        onApply?(readCurve())
        panel.orderOut(nil)
    }

    @objc private func tapReset() {
        configure(curve: MSIFanCurve(), onApply: onApply ?? { _ in })
    }
}

