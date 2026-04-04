import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @State private var viewModel = EmulatorViewModel()
    @State private var showingFilePicker = false
    @State private var pickerMode: PickerMode = .image

    enum PickerMode {
        case source  // picking a folder
        case image   // picking a file
    }

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
                    Label("Open Image", systemImage: "doc")
                        .labelStyle(.titleAndIcon)
                }
                .help("Open a disk image file")
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
                .help(viewModel.isRunning ? "Power off the Lisa" : "Power on the Lisa")
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
            allowedContentTypes: pickerMode == .source ? [.folder] : [.data],
            allowsMultipleSelection: false
        ) { result in
            switch result {
            case .success(let urls):
                guard let url = urls.first else { return }
                if pickerMode == .source {
                    viewModel.log("Selected source folder: \(url.path)")
                    if viewModel.validateSource(url: url) {
                        viewModel.buildFromSource(sourceURL: url)
                    } else {
                        let msg = "Not a valid Lisa_Source folder (expected LISA_OS subdirectory)"
                        viewModel.statusMessage = msg
                        viewModel.log(msg)
                    }
                } else {
                    viewModel.log("Opening disk image: \(url.path)")
                    viewModel.openDiskImage(url: url)
                }
            case .failure(let error):
                let msg = "Error: \(error.localizedDescription)"
                viewModel.statusMessage = msg
                viewModel.log(msg)
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
