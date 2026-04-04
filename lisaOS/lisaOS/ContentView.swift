import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @State private var viewModel = EmulatorViewModel()
    @State private var showingROMPicker = false
    @State private var showingProfilePicker = false
    @State private var showingFloppyPicker = false
    @State private var showingSourcePicker = false
    @State private var showingImagePicker = false
    @State private var showingSavePanel = false
    @State private var pendingSourceURL: URL?

    var body: some View {
        VStack(spacing: 0) {
            LisaDisplayView(viewModel: viewModel)
                .frame(minWidth: 720, minHeight: 364)

            HStack {
                Circle()
                    .fill(viewModel.isRunning ? Color.green : Color.red)
                    .frame(width: 8, height: 8)

                Text(viewModel.statusMessage)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(.secondary)

                Spacer()

                if viewModel.isRunning {
                    Text("Click to capture mouse | Right-click to release")
                        .font(.system(size: 10))
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .background(Color(nsColor: .controlBackgroundColor))
        }
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                // Disk image / build options
                Menu {
                    Button("Build from Source...", systemImage: "hammer") {
                        showingSourcePicker = true
                    }
                    .help("Compile Lisa OS from Apple's released source code")

                    Button("Open Disk Image...", systemImage: "opticaldiscdrive") {
                        showingImagePicker = true
                    }
                    .help("Open a previously built Lisa disk image")

                    Divider()

                    Button("Load ROM...", systemImage: "memorychip") {
                        showingROMPicker = true
                    }

                    Button("Mount ProFile...", systemImage: "externaldrive") {
                        showingProfilePicker = true
                    }

                    Button("Mount Floppy...", systemImage: "opticaldisc") {
                        showingFloppyPicker = true
                    }
                } label: {
                    Label("Disk", systemImage: "internaldrive")
                }

                Divider()

                // Power / Reset
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
                .help(viewModel.isRunning ? "Stop emulation" : "Start emulation")

                Button("Reset", systemImage: "arrow.counterclockwise") {
                    viewModel.reset()
                }
                .help("Reset Lisa")
                .disabled(!viewModel.isRunning)

                Divider()

                Button("Debug", systemImage: "terminal") {
                    viewModel.showDebugger.toggle()
                }
                .help("Toggle CPU debugger")

                // Build status
                if viewModel.isBuilding {
                    ProgressView()
                        .controlSize(.small)
                    Text(viewModel.buildProgress)
                        .font(.system(size: 10))
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
                    pendingSourceURL = url
                    showingSavePanel = true
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
        .fileExporter(
            isPresented: $showingSavePanel,
            document: DiskImageDocument(),
            contentType: .data,
            defaultFilename: "LisaOS.image"
        ) { result in
            if case .success(let saveURL) = result, let sourceURL = pendingSourceURL {
                viewModel.buildFromSource(sourceURL: sourceURL, saveURL: saveURL)
                pendingSourceURL = nil
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

/// Empty document for the save panel
struct DiskImageDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.data] }

    init() {}
    init(configuration: ReadConfiguration) throws {}

    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        FileWrapper(regularFileWithContents: Data())
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
