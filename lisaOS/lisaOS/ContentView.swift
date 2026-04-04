import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @State private var viewModel = EmulatorViewModel()
    @State private var showingROMPicker = false
    @State private var showingProfilePicker = false
    @State private var showingFloppyPicker = false
    @State private var showingSourcePicker = false
    @State private var showingImagePicker = false

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
                } else if viewModel.isRunning {
                    Text("Click to capture mouse")
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
                Button("Build from Source", systemImage: "hammer") {
                    showingSourcePicker = true
                }
                .disabled(viewModel.isBuilding)
            }

            ToolbarItem(placement: .primaryAction) {
                Button("Open Image", systemImage: "doc") {
                    showingImagePicker = true
                }
            }

            ToolbarItem(placement: .primaryAction) {
                Button(
                    viewModel.isRunning ? "Power Off" : "Power On",
                    systemImage: viewModel.isRunning ? "power.circle.fill" : "power.circle"
                ) {
                    if viewModel.isRunning {
                        viewModel.stopEmulation()
                    } else {
                        viewModel.startEmulation()
                    }
                }
            }

            ToolbarItem(placement: .primaryAction) {
                Button("Reset", systemImage: "arrow.counterclockwise") {
                    viewModel.reset()
                }
                .disabled(!viewModel.isRunning)
            }

            ToolbarItem(placement: .primaryAction) {
                Button("Debug", systemImage: "terminal") {
                    viewModel.showDebugger.toggle()
                }
            }
        }
        .fileImporter(isPresented: $showingROMPicker, allowedContentTypes: [.data]) { result in
            if case .success(let url) = result {
                viewModel.loadROM(url: url)
            }
        }
        .fileImporter(isPresented: $showingProfilePicker, allowedContentTypes: [.data]) { result in
            if case .success(let url) = result {
                viewModel.mountProfile(url: url)
            }
        }
        .fileImporter(isPresented: $showingFloppyPicker, allowedContentTypes: [.data]) { result in
            if case .success(let url) = result {
                viewModel.mountFloppy(url: url)
            }
        }
        .fileImporter(isPresented: $showingSourcePicker, allowedContentTypes: [.folder]) { result in
            if case .success(let url) = result {
                if viewModel.validateSource(url: url) {
                    // Use NSSavePanel to let user choose where to save the built image
                    let panel = NSSavePanel()
                    panel.title = "Save Built Disk Image"
                    panel.nameFieldStringValue = "LisaOS.image"
                    panel.allowedContentTypes = [.data]
                    panel.canCreateDirectories = true
                    if panel.runModal() == .OK, let saveURL = panel.url {
                        viewModel.buildFromSource(sourceURL: url, saveURL: saveURL)
                    }
                } else {
                    viewModel.statusMessage = "Invalid: expected Lisa_Source folder with LISA_OS subdirectory"
                }
            }
        }
        .fileImporter(isPresented: $showingImagePicker, allowedContentTypes: [.data]) { result in
            if case .success(let url) = result {
                viewModel.openDiskImage(url: url)
            }
        }
        .sheet(isPresented: $viewModel.showDebugger) {
            DebuggerView(viewModel: viewModel)
        }
        .onAppear {
            viewModel.checkForLastImage()
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
