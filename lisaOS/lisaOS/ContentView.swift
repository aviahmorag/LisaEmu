import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @State private var viewModel = EmulatorViewModel()
    @State private var showingROMPicker = false
    @State private var showingProfilePicker = false
    @State private var showingFloppyPicker = false

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
                Button("Load ROM", systemImage: "memorychip") {
                    showingROMPicker = true
                }
                .help("Load Lisa Boot ROM")

                Button("ProFile", systemImage: "externaldrive") {
                    showingProfilePicker = true
                }
                .help("Mount ProFile Hard Disk Image")

                Button("Floppy", systemImage: "opticaldiscdrive") {
                    showingFloppyPicker = true
                }
                .help("Mount Floppy Disk Image")

                Divider()

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
