import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @State private var viewModel = EmulatorViewModel()
    @State private var showingSourcePicker = false
    @State private var showingImagePicker = false

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
        }
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button {
                    showingSourcePicker = true
                } label: {
                    Label("Build from Source", systemImage: "hammer")
                }
                .help("Select Lisa_Source folder to compile Lisa OS")
                .disabled(viewModel.isBuilding)
            }

            ToolbarItem(placement: .primaryAction) {
                Button {
                    showingImagePicker = true
                } label: {
                    Label("Open Image", systemImage: "doc")
                }
                .help("Open a disk image file")
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
        .fileImporter(
            isPresented: $showingSourcePicker,
            allowedContentTypes: [.folder]
        ) { result in
            if case .success(let url) = result {
                if viewModel.validateSource(url: url) {
                    viewModel.buildFromSource(sourceURL: url)
                } else {
                    viewModel.statusMessage = "Not a valid Lisa_Source folder (expected LISA_OS subdirectory)"
                }
            }
        }
        .fileImporter(
            isPresented: $showingImagePicker,
            allowedContentTypes: [.data]
        ) { result in
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
