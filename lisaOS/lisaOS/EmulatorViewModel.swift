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
        guard romLoaded || buildComplete else {
            statusMessage = "Load a ROM or Build from Source first"
            log("Power On failed: no ROM or built image loaded")
            return
        }

        // Full teardown + re-init so every Power On is a true cold boot.
        // Without this, the second Power On inherits stale CPU, MMU, VIA,
        // and COPS state from the previous run and boot diverges.
        emu_destroy()
        emu_init()

        // Re-load ROM if we have a path (the destroy above cleared it)
        if let romPath = builtRomPath {
            if emu_load_rom(romPath) {
                romLoaded = true
                log("Boot ROM loaded: \(romPath)")
            }
        }

        // Mount image if we have a path
        if let imagePath = builtImagePath {
            _ = emu_mount_profile(imagePath)
            log("Mounted image: \(imagePath)")
        }

        log("Power On — romLoaded=\(romLoaded), buildComplete=\(buildComplete), imagePath=\(builtImagePath ?? "nil")")

        emu_reset()
        romLoaded = true  // emu_reset auto-generates ROM if needed
        log("CPU reset done")
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
        // Restore a "ready to boot" message so the placeholder shows the
        // same welcome text it would on a fresh launch.
        if let imagePath = builtImagePath {
            let name = (imagePath as NSString).lastPathComponent
            statusMessage = "Last image: \(name). Power On to boot."
        } else {
            statusMessage = "Click 'Build from Source' to compile Lisa OS, or load a ROM"
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

    /// Build Lisa OS from source. On success, calls onSuccess so the UI can prompt for save location.
    func buildFromSource(sourceURL: URL, onSuccess: @escaping () -> Void = {}) {
        guard !isBuilding else { return }
        isBuilding = true
        showLogs = true
        buildProgress = "Starting build..."
        statusMessage = "Building..."
        log("Build started from: \(sourceURL.path)")

        let sourcePath = sourceURL.path
        let outputDir = buildCacheDirectory()
        log("Output directory: \(outputDir)")

        let accessing = sourceURL.startAccessingSecurityScopedResource()

        // Use Thread with 8MB stack — the recursive descent parser needs
        // deep stack for INTRINSIC units with complex type declarations.
        let thread = Thread {
            let result = toolchain_build(sourcePath, outputDir, nil)
            if accessing { sourceURL.stopAccessingSecurityScopedResource() }

            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.isBuilding = false

                self.log("Compiled: \(result.files_compiled) Pascal, \(result.files_assembled) assembly, \(result.files_linked) linked, \(result.errors) errors")

                if result.success {
                    let imagePath = "\(outputDir)/lisa_profile.image"
                    let romPath = "\(outputDir)/lisa_boot.rom"
                    self.builtImagePath = imagePath
                    self.buildComplete = true
                    self.buildProgress = "Built: \(result.files_compiled) compiled, \(result.files_assembled) assembled"
                    self.statusMessage = "Build complete! Choose where to save the image."
                    self.log("Build succeeded. Image at: \(imagePath)")

                    // Store ROM path for later loading when image is opened
                    self.builtRomPath = romPath
                    self.log("Build complete. Save the image and open it to boot.")
                    onSuccess()
                } else {
                    var errBuf = result.error_message
                    let errMsg = withUnsafePointer(to: &errBuf) {
                        $0.withMemoryRebound(to: CChar.self, capacity: 512) { String(cString: $0) }
                    }
                    self.buildProgress = "Build failed"
                    self.statusMessage = "Build failed"
                    if errMsg.isEmpty {
                        self.log("Build FAILED: no files compiled (sandbox access issue?)")
                    } else {
                        self.log("Build FAILED: \(errMsg)")
                    }
                }
            }
        }
        thread.stackSize = 8 * 1024 * 1024  // 8MB stack for parser
        thread.start()
    }

    /// Copy the built image to a user-chosen location
    func saveBuiltImage(to url: URL) {
        guard let sourcePath = builtImagePath else {
            log("No built image to save")
            return
        }
        do {
            if FileManager.default.fileExists(atPath: url.path) {
                try FileManager.default.removeItem(at: url)
            }
            try FileManager.default.copyItem(atPath: sourcePath, toPath: url.path)
            log("Image saved to: \(url.path)")
            statusMessage = "Image saved. Power On to boot."
            UserDefaults.standard.set(url.path, forKey: "lastDiskImage")
        } catch {
            log("Failed to save image: \(error.localizedDescription)")
            statusMessage = "Failed to save image"
        }
    }

    private func buildCacheDirectory() -> String {
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let cacheDir = appSupport.appendingPathComponent("LisaEmu/build").path
        try? FileManager.default.createDirectory(atPath: cacheDir, withIntermediateDirectories: true)
        return cacheDir
    }

    /// Open a previously built disk image
    func openDiskImage(url: URL) {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }

        let path = url.path
        // Verify file exists and has content
        guard FileManager.default.fileExists(atPath: path),
              (try? FileManager.default.attributesOfItem(atPath: path))?[.size] as? UInt64 ?? 0 > 0 else {
            statusMessage = "Invalid disk image (empty or missing)"
            log("Invalid disk image (empty or missing): \(path)")
            return
        }
        if emu_mount_profile(path) {
            builtImagePath = path
            buildComplete = true
            statusMessage = "Disk image loaded: \(url.lastPathComponent). Power On to boot."
            log("Disk image loaded: \(path)")
            // Save bookmark for sandbox access on next launch
            if let bookmark = try? url.bookmarkData(options: .withSecurityScope, includingResourceValuesForKeys: nil, relativeTo: nil) {
                UserDefaults.standard.set(bookmark, forKey: "lastDiskImageBookmark")
            }
            UserDefaults.standard.set(path, forKey: "lastDiskImage")
        } else {
            statusMessage = "Failed to open disk image"
            log("Failed to open disk image: \(path)")
        }
    }

    /// Restore the last used disk image on launch
    func checkForLastImage() {
        guard let bookmarkData = UserDefaults.standard.data(forKey: "lastDiskImageBookmark") else { return }
        var isStale = false
        guard let url = try? URL(resolvingBookmarkData: bookmarkData, options: .withSecurityScope, relativeTo: nil, bookmarkDataIsStale: &isStale) else { return }

        let accessing = url.startAccessingSecurityScopedResource()
        log("Restoring: \(url.path)")

        if emu_mount_profile(url.path) {
            builtImagePath = url.path
            buildComplete = true
            statusMessage = "Last image: \(url.lastPathComponent). Power On to boot."
            log("Mounted: \(url.lastPathComponent)")

            // Load ROM from same directory if available
            let romPath = url.deletingLastPathComponent().appendingPathComponent("lisa_boot.rom").path
            if FileManager.default.fileExists(atPath: romPath) {
                romLoaded = emu_load_rom(romPath)
                log("ROM: \(romLoaded)")
            }
        } else {
            log("Failed to mount last image")
            if accessing { url.stopAccessingSecurityScopedResource() }
        }
    }

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
