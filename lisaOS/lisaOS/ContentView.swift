import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @State private var viewModel = EmulatorViewModel()
    @State private var showingFilePicker = false
    @State private var pickerMode: PickerMode = .image
    @State private var pendingSourceURL: URL?

    enum PickerMode {
        case source       // picking Lisa_Source folder
        case output       // picking output folder (after source is chosen)
        case image        // picking a .lisa system bundle
    }

    // ".lisa" system bundle type — a folder package containing profile.image +
    // build byproducts. Declared at runtime as conforming to .package so the
    // Open dialog treats it as a single selectable item (no drill-in).
    // For proper Finder package treatment across the OS, add a matching UTI
    // declaration to Info.plist later.
    static let lisaBundleType: UTType = {
        UTType(filenameExtension: "lisa", conformingTo: .package)
            ?? UTType(filenameExtension: "lisa")
            ?? .package
    }()

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

            // Logs panel
            if viewModel.showLogs {
                VStack(alignment: .leading, spacing: 0) {
                    HStack {
                        Text("Logs")
                            .font(.system(size: 11, weight: .semibold))
                        Spacer()
                        Button("Copy") {
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(viewModel.logText, forType: .string)
                        }
                        .controlSize(.small)
                        Button("Clear") {
                            viewModel.clearLogs()
                        }
                        .controlSize(.small)
                    }
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)

                    ScrollView {
                        Text(viewModel.logText.isEmpty ? "No logs yet." : viewModel.logText)
                            .font(.system(size: 11, design: .monospaced))
                            .foregroundStyle(viewModel.logText.isEmpty ? .secondary : .primary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .textSelection(.enabled)
                            .padding(8)
                    }
                    .frame(height: 150)
                    .background(Color(nsColor: .textBackgroundColor))
                }
                .background(Color(nsColor: .controlBackgroundColor))
            }
        }
        .toolbar {
            ToolbarItem(placement: .automatic) {
                Button {
                    pickerMode = .source
                    showingFilePicker = true
                } label: {
                    Label("Build from Source", systemImage: "hammer")
                        .labelStyle(.titleAndIcon)
                }
                .help("Select Lisa_Source folder to compile Lisa OS")
                .disabled(viewModel.isBuilding)
            }

            ToolbarItem(placement: .automatic) {
                Button {
                    pickerMode = .image
                    showingFilePicker = true
                } label: {
                    Label("Open System", systemImage: "doc")
                        .labelStyle(.titleAndIcon)
                }
                .help("Open a .lisa system bundle (built with Build from Source)")
            }

            ToolbarItem(placement: .automatic) {
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
                    .labelStyle(.titleAndIcon)
                }
                // Power On requires an image to be loaded (either from
                // "Build from Source" or "Open Image"). Disable otherwise —
                // no point firing up the CPU with nothing to run.
                .disabled(!viewModel.isRunning && !viewModel.buildComplete)
                .help(viewModel.isRunning
                      ? "Power off the Lisa"
                      : (viewModel.buildComplete
                         ? "Power on the Lisa"
                         : "Build from Source or Open Image first"))
            }

            ToolbarItem(placement: .automatic) {
                Button {
                    viewModel.reset()
                } label: {
                    Label("Reset", systemImage: "arrow.counterclockwise")
                        .labelStyle(.titleAndIcon)
                }
                .help("Reset the Lisa")
                .disabled(!viewModel.isRunning)
            }

            ToolbarItem(placement: .automatic) {
                Button {
                    viewModel.showDebugger.toggle()
                } label: {
                    Label("Debug", systemImage: "terminal")
                        .labelStyle(.titleAndIcon)
                }
                .help("CPU debugger")
            }

            ToolbarItem(placement: .automatic) {
                Button {
                    viewModel.showLogs.toggle()
                } label: {
                    Label("Logs", systemImage: "doc.text")
                        .labelStyle(.titleAndIcon)
                }
                .help("Show build and emulation logs")
            }
        }
        .fileImporter(
            isPresented: $showingFilePicker,
            allowedContentTypes: {
                switch pickerMode {
                case .source, .output: return [.folder]
                case .image:           return [Self.lisaBundleType]
                }
            }(),
            allowsMultipleSelection: false
        ) { result in
            switch result {
            case .success(let urls):
                guard let url = urls.first else { return }
                switch pickerMode {
                case .source:
                    viewModel.log("Selected source folder: \(url.path)")
                    if viewModel.validateSource(url: url) {
                        // Hold the source URL and prompt for output folder next.
                        pendingSourceURL = url
                        pickerMode = .output
                        // Re-open the picker asynchronously so SwiftUI
                        // processes the sheet dismissal before we raise it again.
                        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                            showingFilePicker = true
                        }
                    } else {
                        let msg = "Not a valid Lisa_Source folder (expected LISA_OS subdirectory)"
                        viewModel.statusMessage = msg
                        viewModel.log(msg)
                        pendingSourceURL = nil
                    }
                case .output:
                    guard let srcURL = pendingSourceURL else {
                        viewModel.log("No source folder pending — click Build from Source first")
                        return
                    }
                    pendingSourceURL = nil
                    viewModel.log("Selected output folder: \(url.path)")
                    viewModel.buildFromSource(sourceURL: srcURL, outputURL: url)
                case .image:
                    viewModel.log("Opening disk image: \(url.path)")
                    viewModel.openDiskImage(url: url)
                }
            case .failure(let error):
                let msg = "Error: \(error.localizedDescription)"
                viewModel.statusMessage = msg
                viewModel.log(msg)
                pendingSourceURL = nil
            }
        }
        .sheet(isPresented: $viewModel.showDebugger) {
            DebuggerView(viewModel: viewModel)
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

            ScrollView {
                Text(viewModel.cpuState)
                    .font(.system(size: 12, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
            }
            .padding(8)
            .background(Color.black.opacity(0.8))
            .foregroundStyle(.green)
            .clipShape(RoundedRectangle(cornerRadius: 4))

            HStack {
                Button("Copy") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(viewModel.cpuState, forType: .string)
                }
                .buttonStyle(.bordered)

                Button("Step") {
                    _ = emu_run_frame()
                    viewModel.updateCPUState()
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
