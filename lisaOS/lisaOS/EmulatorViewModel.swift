import SwiftUI
import Observation

@Observable
@MainActor
final class EmulatorViewModel {
    var displayImage: CGImage?
    var isRunning = false
    var cpuState = ""
    var romLoaded = false
    var statusMessage = "Click 'Build from Source' to compile Lisa OS, or load a ROM"
    var showDebugger = false

    // Build from source
    var isBuilding = false
    var buildProgress = ""
    var buildComplete = false
    var builtImagePath: String?
    var builtRomPath: String?

    // Logs
    var showLogs = false
    var logText = ""

    private var emulatorTimer: Timer?

    let screenWidth = Int(emu_screen_width())
    let screenHeight = Int(emu_screen_height())

    init() {
        emu_init()
        // Purge obsolete UserDefaults keys from earlier sandbox-era builds.
        // The app is no longer sandboxed and stores nothing across sessions
        // about paths; every launch starts with no remembered state. File
        // paths are derived from whatever the user picks in the current
        // session — see openDiskImage/buildFromSource.
        for k in ["lastDiskImageBookmark", "lastDiskImage",
                  "lastSourceBookmark", "lastOutputBookmark"] {
            UserDefaults.standard.removeObject(forKey: k)
        }
    }

    func cleanup() {
        emulatorTimer?.invalidate()
        emulatorTimer = nil
        emu_destroy()
    }

    func loadROM(url: URL) {
        let path = url.path
        if emu_load_rom(path) {
            romLoaded = true
            statusMessage = "ROM loaded: \(url.lastPathComponent)"
            log("ROM loaded: \(path)")
        } else {
            statusMessage = "Failed to load ROM"
            log("Failed to load ROM: \(path)")
        }
    }

    func mountProfile(url: URL) {
        let path = url.path
        if emu_mount_profile(path) {
            statusMessage = "ProFile mounted: \(url.lastPathComponent)"
            log("ProFile mounted: \(path)")
        } else {
            statusMessage = "Failed to mount ProFile image"
            log("Failed to mount ProFile: \(path)")
        }
    }

    func mountFloppy(url: URL) {
        let path = url.path
        if emu_mount_floppy(path) {
            statusMessage = "Floppy mounted: \(url.lastPathComponent)"
            log("Floppy mounted: \(path)")
        } else {
            statusMessage = "Failed to mount floppy image"
            log("Failed to mount floppy: \(path)")
        }
    }

    func startEmulation() {
        guard buildComplete else {
            statusMessage = "Build from Source or Open System first"
            log("Power On failed: no image loaded")
            return
        }

        // Full teardown + re-init so every Power On is a true cold boot.
        // Without this, the second Power On inherits stale CPU, MMU, VIA,
        // and COPS state from the previous run and boot diverges.
        emu_destroy()
        emu_init()

        // Re-load ROM. Sandbox is off, so this is just a plain fopen.
        romLoaded = false
        if let romPath = builtRomPath, emu_load_rom(romPath) {
            romLoaded = true
            log("Boot ROM loaded: \(romPath)")
        } else if let romPath = builtRomPath {
            log("⚠️ Failed to load ROM at \(romPath)")
        } else {
            log("⚠️ No ROM path set — this session has no ROM to load.")
        }

        // Mount image
        var imageMounted = false
        if let imagePath = builtImagePath {
            if emu_mount_profile(imagePath) {
                imageMounted = true
                log("Mounted image: \(imagePath)")
            } else {
                log("⚠️ Failed to mount image: \(imagePath)")
            }
        } else {
            log("⚠️ No builtImagePath — ProFile disk is empty")
        }

        // Wire HLE intercepts (CALLDRIVER, SYSTEM_ERROR, loader traps, ...)
        // and load the symbol map for boot-progress tracking + dynamic HLE
        // lookups (CreateProcess, Make_SProcess, etc.). Both files live
        // inside the .lisa bundle. Without both, the kernel's intercepts
        // and dynamic symbol resolution are dead and the CPU walks off
        // during boot. The SDL harness does the same two steps.
        if let imagePath = builtImagePath {
            let bundleDir = (imagePath as NSString).deletingLastPathComponent
            let hlePath = bundleDir + "/hle_addrs.txt"
            let mapPath = bundleDir + "/linked.map"
            if emu_load_hle_addrs(hlePath) {
                log("HLE wired from: \(hlePath)")
            } else {
                log("⚠️ HLE wiring failed (\(hlePath))")
            }
            if emu_load_symbol_map(mapPath) {
                log("Symbol map loaded: \(mapPath)")
            } else {
                log("⚠️ Symbol map load failed (\(mapPath)) — dynamic HLE lookups will fall back to hardcoded values.")
            }
        }

        // Hard gate: refuse to Power On without a real ROM. The emulator
        // side no longer auto-generates a stub, so the choice is load a ROM
        // or don't boot — no silent fake fallback.
        guard emu_has_rom() else {
            log("⛔ Power On aborted — no boot ROM loaded.")
            log("   Build from Source to produce one, or open a .lisa bundle whose sibling rom/lisa_boot.rom exists.")
            statusMessage = "No boot ROM — can't Power On"
            return
        }
        if !imageMounted {
            log("⚠️ Power On with no disk image mounted — boot ROM will run but the kernel load will fail.")
        }

        emu_reset()
        log("CPU reset done (ROM: \(builtRomPath ?? "?"))")
        isRunning = true
        statusMessage = "Running"

        // Run first frame and update display immediately
        let firstFrame = emu_run_frame()
        log("First frame result: \(firstFrame)")
        updateDisplay()

        emulatorTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            guard let vm = self else { return }
            Task { @MainActor in
                vm.runFrame()
            }
        }
    }

    func stopEmulation() {
        emulatorTimer?.invalidate()
        emulatorTimer = nil
        isRunning = false
        emu_set_running(false)
        displayImage = nil
        if let imagePath = builtImagePath {
            let name = (imagePath as NSString).lastPathComponent
            statusMessage = "System: \(name). Power On to boot."
        } else {
            statusMessage = "Click 'Build from Source' to compile Lisa OS, or open a .lisa"
        }
        log("Power Off")
    }

    func togglePause() {
        if isRunning {
            stopEmulation()
        } else if romLoaded {
            isRunning = true
            emu_set_running(true)
            statusMessage = "Running"
            emulatorTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
                guard let vm = self else { return }
                Task { @MainActor in
                    vm.runFrame()
                }
            }
        }
    }

    func reset() {
        stopEmulation()
        if romLoaded {
            startEmulation()
        }
    }

    private func runFrame() {
        guard isRunning else { return }

        _ = emu_run_frame()
        updateDisplay()

        if showDebugger {
            updateCPUState()
        }
    }

    private func updateDisplay() {
        guard let fbPointer = emu_get_framebuffer() else { return }

        let width = screenWidth
        let height = screenHeight
        let bytesPerRow = width * 4

        let data = Data(bytes: fbPointer, count: width * height * 4)

        guard let provider = CGDataProvider(data: data as CFData) else { return }

        let colorSpace = CGColorSpaceCreateDeviceRGB()
        let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)

        displayImage = CGImage(
            width: width,
            height: height,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: bytesPerRow,
            space: colorSpace,
            bitmapInfo: bitmapInfo,
            provider: provider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        )
    }

    func updateCPUState() {
        var buffer = [CChar](repeating: 0, count: 1024)
        emu_get_cpu_state(&buffer, 1024)
        cpuState = String(cString: buffer)
    }

    // MARK: - Build from Source

    /// Build Lisa OS from source directly into a user-chosen output folder.
    /// Toolchain produces:
    ///   <output>/LisaOS.lisa/profile.image        (the bootable disk)
    ///   <output>/LisaOS.lisa/linked.bin           (linker raw output)
    ///   <output>/LisaOS.lisa/linked.map           (symbol map)
    ///   <output>/LisaOS.lisa/hle_addrs.txt        (HLE wiring)
    ///   <output>/rom/lisa_boot.rom                (shared boot ROM)
    /// .lisa is a macOS bundle package (folder that presents as one file).
    /// The ROM lives outside the bundle so it's reusable across any bundle.
    func buildFromSource(sourceURL: URL, outputURL: URL) {
        guard !isBuilding else { return }
        isBuilding = true
        showLogs = true
        buildProgress = "Starting build..."
        statusMessage = "Building..."
        log("Build started from: \(sourceURL.path)")
        log("Output folder: \(outputURL.path)")

        let sourcePath = sourceURL.path
        let outputDir = outputURL.path

        // Sandbox is off (ENABLE_APP_SANDBOX = NO), so paths from the file
        // picker are plain strings. No bookmarks, no security scopes.

        // Use Thread with 8MB stack — the recursive descent parser needs
        // deep stack for INTRINSIC units with complex type declarations.
        let thread = Thread {
            let result = toolchain_build(sourcePath, outputDir, nil)

            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.isBuilding = false

                self.log("Compiled: \(result.files_compiled) Pascal, \(result.files_assembled) assembly, \(result.files_linked) linked, \(result.errors) errors")

                if result.success {
                    let bundlePath = "\(outputDir)/LisaOS.lisa"
                    let imagePath = "\(bundlePath)/profile.image"
                    let romPath = "\(outputDir)/rom/lisa_boot.rom"
                    self.builtImagePath = imagePath
                    self.builtRomPath = romPath
                    self.buildComplete = true
                    self.buildProgress = "Built: \(result.files_compiled) compiled, \(result.files_assembled) assembled"
                    self.statusMessage = "Build complete — Power On to boot."
                    self.log("Build succeeded. Image: \(imagePath)")
                    self.log("Build succeeded. ROM:   \(romPath)")
                } else {
                    var errBuf = result.error_message
                    let errMsg = withUnsafePointer(to: &errBuf) {
                        $0.withMemoryRebound(to: CChar.self, capacity: 512) { String(cString: $0) }
                    }
                    self.buildProgress = "Build failed"
                    self.statusMessage = "Build failed"
                    self.log(errMsg.isEmpty ? "Build FAILED: no files compiled" : "Build FAILED: \(errMsg)")
                }
            }
        }
        thread.stackSize = 8 * 1024 * 1024  // 8MB stack for parser
        thread.start()
    }

    /// Open a `.lisa` system bundle. The bundle is a folder containing
    /// `profile.image` (+ build byproducts). The ROM lives beside it at
    /// `<bundleParent>/rom/lisa_boot.rom` — same parent folder that was
    /// the "output" folder the last time someone ran Build from Source.
    /// All paths are derived from the URL the user just picked; nothing
    /// is remembered across sessions.
    func openDiskImage(url: URL) {
        let imagePath = url.appendingPathComponent("profile.image").path
        guard FileManager.default.fileExists(atPath: imagePath),
              (try? FileManager.default.attributesOfItem(atPath: imagePath))?[.size] as? UInt64 ?? 0 > 0 else {
            statusMessage = "Not a valid .lisa bundle (missing profile.image)"
            log("⚠️ Not a valid .lisa bundle — \(url.path) has no profile.image")
            return
        }
        guard emu_mount_profile(imagePath) else {
            statusMessage = "Failed to mount disk image from bundle"
            log("Failed to mount \(imagePath)")
            return
        }

        builtImagePath = imagePath
        buildComplete = true
        statusMessage = "System loaded: \(url.lastPathComponent). Power On to boot."
        log("System bundle loaded: \(url.path)")

        // ROM path is fully derived from the bundle's parent folder — the
        // "output" folder convention. No bookmarks, no UserDefaults, just
        // path arithmetic against the URL the user just clicked.
        let romPath = url.deletingLastPathComponent()
            .appendingPathComponent("rom/lisa_boot.rom").path
        if FileManager.default.fileExists(atPath: romPath),
           emu_load_rom(romPath) {
            builtRomPath = romPath
            romLoaded = true
            log("Boot ROM loaded: \(romPath)")
        } else {
            log("⚠️ No ROM at \(romPath) — run Build from Source to produce one.")
            builtRomPath = nil
            romLoaded = false
        }
    }

    // (No checkForLastImage — auto-restore of the last disk image is
    //  intentionally NOT implemented. Every launch starts clean; the
    //  user has asked for this behavior repeatedly. See
    //  feedback_no_auto_restore.md.)

    func log(_ message: String) {
        let timestamp = DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
        logText += "[\(timestamp)] \(message)\n"
    }

    func clearLogs() {
        logText = ""
    }

    /// Validate a source directory
    func validateSource(url: URL) -> Bool {
        return toolchain_validate_source(url.path)
    }

    // MARK: - Input

    func keyDown(_ keyCode: UInt16) {
        let lisaKeycode = mapKeyCode(keyCode)
        if lisaKeycode >= 0 {
            emu_key_down(Int32(lisaKeycode))
        }
    }

    func keyUp(_ keyCode: UInt16) {
        let lisaKeycode = mapKeyCode(keyCode)
        if lisaKeycode >= 0 {
            emu_key_up(Int32(lisaKeycode))
        }
    }

    func mouseMove(dx: Int, dy: Int) {
        emu_mouse_move(Int32(dx), Int32(dy))
    }

    func mouseDown() {
        emu_mouse_button(true)
    }

    func mouseUp() {
        emu_mouse_button(false)
    }

    /// Map a macOS virtual keyCode (kVK_ANSI_*) to a Lisa physical keycode.
    /// Lisa keycodes come from Lisa_Source/LISA_OS/LIBS/LIBHW/LIBHW-LEGENDS.TEXT
    /// (Final US layout, Primary section at keycode $40..$7F).
    private func mapKeyCode(_ macKeyCode: UInt16) -> Int {
        let keyMap: [UInt16: Int] = [
            // Letters
            0x00: 0x70, // A
            0x0B: 0x6E, // B
            0x08: 0x6D, // C
            0x02: 0x7B, // D
            0x0E: 0x60, // E
            0x03: 0x69, // F
            0x05: 0x6A, // G
            0x04: 0x6B, // H
            0x22: 0x53, // I
            0x26: 0x54, // J
            0x28: 0x55, // K
            0x25: 0x59, // L
            0x2E: 0x58, // M
            0x2D: 0x6F, // N
            0x1F: 0x5F, // O
            0x23: 0x44, // P
            0x0C: 0x75, // Q
            0x0F: 0x65, // R
            0x01: 0x76, // S
            0x11: 0x66, // T
            0x20: 0x52, // U
            0x09: 0x6C, // V
            0x0D: 0x77, // W
            0x07: 0x7A, // X
            0x10: 0x67, // Y
            0x06: 0x79, // Z

            // Top-row digits
            0x12: 0x74, // 1
            0x13: 0x71, // 2
            0x14: 0x72, // 3
            0x15: 0x73, // 4
            0x17: 0x64, // 5
            0x16: 0x61, // 6
            0x1A: 0x62, // 7
            0x1C: 0x63, // 8
            0x19: 0x50, // 9
            0x1D: 0x51, // 0

            // Punctuation / symbols
            0x1B: 0x40, // -
            0x18: 0x41, // =
            0x2A: 0x42, // \
            0x21: 0x56, // [
            0x1E: 0x57, // ]
            0x29: 0x5A, // ;
            0x27: 0x5B, // '
            0x2B: 0x5D, // ,
            0x2F: 0x5E, // .
            0x2C: 0x4C, // / ?
            0x32: 0x68, // `

            // Control keys
            0x31: 0x5C, // Space
            0x24: 0x48, // Return
            0x30: 0x78, // Tab
            0x33: 0x45, // BackSpace (Delete)
            0x35: 0x20, // Esc → Clear

            // Modifiers — Lisa treats these as real keycodes
            0x38: 0x7E, // Left Shift
            0x3C: 0x7E, // Right Shift
            0x39: 0x7D, // Caps Lock
            0x3A: 0x7C, // Left Option
            0x3D: 0x7C, // Right Option (treat as L-Option)
            0x37: 0x7F, // Left Command (Apple/Command)
            0x36: 0x7F, // Right Command

            // Arrow keys
            0x7B: 0x22, // Left
            0x7C: 0x23, // Right
            0x7D: 0x2B, // Down
            0x7E: 0x27, // Up
        ]
        return keyMap[macKeyCode] ?? -1
    }
}
