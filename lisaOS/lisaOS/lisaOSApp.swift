import SwiftUI

@main
struct lisaOSApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        .windowStyle(.titleBar)
        .defaultSize(width: 900, height: 520)
        .commands {
            CommandGroup(replacing: .newItem) {}

            CommandMenu("Machine") {
                Button("Power On/Off") {
                    // Handled via toolbar
                }
                .keyboardShortcut("p", modifiers: [.command, .shift])

                Button("Reset") {
                    // Handled via toolbar
                }
                .keyboardShortcut("r", modifiers: [.command, .shift])

                Divider()

                Button("Load ROM...") {}
                    .keyboardShortcut("o", modifiers: [.command])

                Button("Mount ProFile...") {}
                    .keyboardShortcut("h", modifiers: [.command, .shift])

                Button("Mount Floppy...") {}
                    .keyboardShortcut("f", modifiers: [.command, .shift])
            }

            CommandMenu("Debug") {
                Button("Toggle Debugger") {}
                    .keyboardShortcut("d", modifiers: [.command, .shift])
            }
        }
    }
}
