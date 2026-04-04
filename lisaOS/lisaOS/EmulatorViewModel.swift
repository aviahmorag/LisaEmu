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
        } else {
            statusMessage = "Failed to load ROM"
        }
    }

    func mountProfile(url: URL) {
        let path = url.path
        if emu_mount_profile(path) {
            statusMessage = "ProFile mounted: \(url.lastPathComponent)"
        } else {
            statusMessage = "Failed to mount ProFile image"
        }
    }

    func mountFloppy(url: URL) {
        let path = url.path
        if emu_mount_floppy(path) {
            statusMessage = "Floppy mounted: \(url.lastPathComponent)"
        } else {
            statusMessage = "Failed to mount floppy image"
        }
    }

    func startEmulation() {
        guard romLoaded || buildComplete else {
            statusMessage = "Load a ROM or Build from Source first"
            return
        }

        emu_reset()
        isRunning = true
        statusMessage = "Running"

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
        statusMessage = "Stopped"
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

    private func updateCPUState() {
        var buffer = [CChar](repeating: 0, count: 1024)
        emu_get_cpu_state(&buffer, 1024)
        cpuState = String(cString: buffer)
    }

    // MARK: - Build from Source

    /// Build Lisa OS from source and save to user-chosen location
    func buildFromSource(sourceURL: URL, saveURL: URL) {
        guard !isBuilding else { return }
        isBuilding = true
        buildProgress = "Starting build..."
        statusMessage = "Building..."

        let sourcePath = sourceURL.path
        let imageName = saveURL.lastPathComponent

        Task.detached {
            // Build to a temp directory, then move to final location
            let tempDir = NSTemporaryDirectory() + "LisaEmu_build_\(ProcessInfo.processInfo.globallyUniqueString)"
            try? FileManager.default.createDirectory(atPath: tempDir, withIntermediateDirectories: true)

            let result = toolchain_build(sourcePath, tempDir, nil)

            await MainActor.run { [weak self] in
                guard let self else { return }
                self.isBuilding = false

                if result.success {
                    // Move built image to user's chosen location
                    let builtPath = "\(tempDir)/lisa_profile.image"
                    let finalPath = saveURL.path
                    do {
                        if FileManager.default.fileExists(atPath: finalPath) {
                            try FileManager.default.removeItem(atPath: finalPath)
                        }
                        try FileManager.default.moveItem(atPath: builtPath, toPath: finalPath)
                        self.builtImagePath = finalPath
                        self.buildComplete = true
                        self.buildProgress = "Built: \(result.files_compiled) compiled, \(result.files_assembled) assembled"
                        self.statusMessage = "Build saved to \(imageName). Power On to boot."

                        // Auto-mount
                        _ = emu_mount_profile(finalPath)

                        // Remember last image path
                        UserDefaults.standard.set(finalPath, forKey: "lastDiskImage")
                    } catch {
                        self.buildProgress = "Build succeeded but failed to save: \(error.localizedDescription)"
                        self.statusMessage = "Save failed"
                    }
                    // Clean up temp
                    try? FileManager.default.removeItem(atPath: tempDir)
                } else {
                    var errBuf = result.error_message
                    let errMsg = withUnsafePointer(to: &errBuf) {
                        $0.withMemoryRebound(to: CChar.self, capacity: 512) { String(cString: $0) }
                    }
                    self.buildProgress = "Build failed: \(errMsg)"
                    self.statusMessage = "Build failed"
                    try? FileManager.default.removeItem(atPath: tempDir)
                }
            }
        }
    }

    /// Open a previously built disk image
    func openDiskImage(url: URL) {
        let path = url.path
        if emu_mount_profile(path) {
            builtImagePath = path
            buildComplete = true
            statusMessage = "Disk image loaded: \(url.lastPathComponent). Power On to boot."
            UserDefaults.standard.set(path, forKey: "lastDiskImage")
        } else {
            statusMessage = "Failed to open disk image"
        }
    }

    /// Check if a previously used disk image exists
    func checkForLastImage() {
        if let lastPath = UserDefaults.standard.string(forKey: "lastDiskImage"),
           FileManager.default.fileExists(atPath: lastPath) {
            builtImagePath = lastPath
            buildComplete = true
            let name = URL(fileURLWithPath: lastPath).lastPathComponent
            statusMessage = "Last image: \(name). Power On to boot."
            _ = emu_mount_profile(lastPath)
        }
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

    private func mapKeyCode(_ macKeyCode: UInt16) -> Int {
        let keyMap: [UInt16: Int] = [
            0x00: 0x42, 0x01: 0x56, 0x02: 0x57, 0x03: 0x58,
            0x04: 0x44, 0x05: 0x59, 0x06: 0x68, 0x07: 0x69,
            0x08: 0x6A, 0x09: 0x6B, 0x0B: 0x5A, 0x0C: 0x30,
            0x0D: 0x31, 0x0E: 0x32, 0x0F: 0x33, 0x10: 0x35,
            0x11: 0x34, 0x12: 0x20, 0x13: 0x21, 0x14: 0x22,
            0x15: 0x23, 0x16: 0x25, 0x17: 0x24, 0x18: 0x26,
            0x19: 0x28, 0x1A: 0x27, 0x1B: 0x29, 0x1C: 0x2A,
            0x1D: 0x2B, 0x1E: 0x38, 0x1F: 0x36, 0x20: 0x43,
            0x21: 0x37, 0x22: 0x45, 0x23: 0x46, 0x24: 0x48,
            0x25: 0x5B, 0x26: 0x5C, 0x27: 0x4A, 0x28: 0x5D,
            0x29: 0x4B, 0x2A: 0x4C, 0x2B: 0x4D, 0x2C: 0x6C,
            0x2D: 0x5E, 0x2E: 0x5F, 0x2F: 0x6D,
            0x30: 0x48, 0x31: 0x6E, 0x33: 0x2D, 0x35: 0x00,
            0x38: 0x70, 0x3A: 0x72, 0x37: 0x74, 0x3C: 0x71,
            0x36: 0x74,
            0x7B: 0x01, 0x7C: 0x02, 0x7D: 0x03, 0x7E: 0x04,
        ]
        return keyMap[macKeyCode] ?? -1
    }
}
