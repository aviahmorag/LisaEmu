import SwiftUI
import AppKit

struct ContentView: View {
    @State private var viewModel = EmulatorViewModel()

    var body: some View {
        VStack(spacing: 0) {
            LisaDisplayView(viewModel: viewModel)
                .frame(minWidth: 720, minHeight: 364)

            // Status bar
            HStack {
                Circle()
                    .fill(viewModel.isRunning ? Color.green : Color.red)
                    .frame(width: 8, height: 8)

                Text(viewModel.statusMessage)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)

                Spacer()

                if viewModel.isBuilding {
                    ProgressView()
                        .controlSize(.small)
                    Text(viewModel.buildProgress)
                        .font(.system(size: 10))
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .background(Color(nsColor: .controlBackgroundColor))
        }
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button {
                    openFileOrFolder()
                } label: {
                    Label("Open", systemImage: "folder")
                }
                .help("Open a Lisa_Source folder to build, or a disk image to boot")
                .disabled(viewModel.isBuilding)
            }

            ToolbarItem(placement: .primaryAction) {
                Button {
                    if viewModel.isRunning {
                        viewModel.stopEmulation()
                    } else {
                        viewModel.startEmulation()
                    }
                } label: {
                    Label(
                        viewModel.isRunning ? "Power Off" : "Power On",
                        systemImage: viewModel.isRunning ? "power.circle.fill" : "power.circle"
                    )
                }
                .help(viewModel.isRunning ? "Power off the Lisa" : "Power on the Lisa")
            }

            ToolbarItem(placement: .primaryAction) {
                Button {
                    viewModel.reset()
                } label: {
                    Label("Reset", systemImage: "arrow.counterclockwise")
                }
                .help("Reset the Lisa")
                .disabled(!viewModel.isRunning)
            }

            ToolbarItem(placement: .primaryAction) {
                Button {
                    viewModel.showDebugger.toggle()
                } label: {
                    Label("Debug", systemImage: "terminal")
                }
                .help("CPU debugger")
            }
        }
        .sheet(isPresented: $viewModel.showDebugger) {
            DebuggerView(viewModel: viewModel)
        }
        .onAppear {
            viewModel.checkForLastImage()
        }
    }

    /// Open dialog that accepts both folders (source) and files (disk images)
    private func openFileOrFolder() {
        let panel = NSOpenPanel()
        panel.title = "Open Lisa Source Folder or Disk Image"
        panel.message = "Select a Lisa_Source folder to build from source, or a .image file to boot directly."
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false

        panel.begin { response in
            guard response == .OK, let url = panel.url else { return }

            var isDir: ObjCBool = false
            FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir)

            if isDir.boolValue {
                if viewModel.validateSource(url: url) {
                    // Defer the save panel to next run loop iteration
                    DispatchQueue.main.async {
                        let save = NSSavePanel()
                        save.title = "Save Built Disk Image"
                        save.nameFieldStringValue = "LisaOS.image"
                        save.canCreateDirectories = true
                        save.begin { saveResponse in
                            guard saveResponse == .OK, let saveURL = save.url else { return }
                            viewModel.buildFromSource(sourceURL: url, saveURL: saveURL)
                        }
                    }
                } else {
                    viewModel.statusMessage = "Not a valid Lisa_Source folder (expected LISA_OS subdirectory)"
                }
            } else {
                viewModel.openDiskImage(url: url)
            }
        }
    }
}

struct DebuggerView: View {
    var viewModel: EmulatorViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("CPU Debugger")
                    .font(.headline)
                Spacer()
                Button("Close") {
                    viewModel.showDebugger = false
                }
            }

            Text(viewModel.cpuState)
                .font(.system(size: 12, design: .monospaced))
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(8)
                .background(Color.black.opacity(0.8))
                .foregroundStyle(.green)
                .clipShape(RoundedRectangle(cornerRadius: 4))

            HStack {
                Button("Step") {
                    _ = emu_run_frame()
                }
                .buttonStyle(.bordered)

                Button("Pause/Resume") {
                    viewModel.togglePause()
                }
                .buttonStyle(.bordered)
            }
        }
        .padding()
        .frame(minWidth: 500, minHeight: 300)
    }
}

#Preview {
    ContentView()
}
