import SwiftUI
import AppKit

final class LisaDisplayNSView: NSView {
    var viewModel: EmulatorViewModel?
    private var activeTrackingArea: NSTrackingArea?
    var isCapturingMouse = false

    override var acceptsFirstResponder: Bool { true }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let existing = activeTrackingArea {
            removeTrackingArea(existing)
        }
        let area = NSTrackingArea(
            rect: bounds,
            options: [.activeInKeyWindow, .mouseMoved, .mouseEnteredAndExited, .inVisibleRect],
            owner: self,
            userInfo: nil
        )
        addTrackingArea(area)
        activeTrackingArea = area
    }

    override func keyDown(with event: NSEvent) {
        viewModel?.keyDown(event.keyCode)
    }

    override func keyUp(with event: NSEvent) {
        viewModel?.keyUp(event.keyCode)
    }

    override func mouseDown(with event: NSEvent) {
        if !isCapturingMouse {
            isCapturingMouse = true
            NSCursor.hide()
            CGAssociateMouseAndMouseCursorPosition(0)
        }
        viewModel?.mouseDown()
    }

    override func mouseUp(with event: NSEvent) {
        viewModel?.mouseUp()
    }

    override func rightMouseDown(with event: NSEvent) {
        if isCapturingMouse {
            isCapturingMouse = false
            NSCursor.unhide()
            CGAssociateMouseAndMouseCursorPosition(1)
        }
    }

    override func mouseMoved(with event: NSEvent) {
        guard isCapturingMouse else { return }
        viewModel?.mouseMove(dx: Int(event.deltaX), dy: Int(event.deltaY))
    }

    override func mouseDragged(with event: NSEvent) {
        guard isCapturingMouse else { return }
        viewModel?.mouseMove(dx: Int(event.deltaX), dy: Int(event.deltaY))
    }

    override func flagsChanged(with event: NSEvent) {
        let keyCode = event.keyCode
        let flags = event.modifierFlags

        // Ctrl+Option releases captured mouse
        if isCapturingMouse && flags.contains(.control) && flags.contains(.option) {
            isCapturingMouse = false
            NSCursor.unhide()
            CGAssociateMouseAndMouseCursorPosition(1)
            return
        }

        switch keyCode {
        case 56, 60:
            if flags.contains(.shift) { viewModel?.keyDown(keyCode) }
            else { viewModel?.keyUp(keyCode) }
        case 58, 61:
            if flags.contains(.option) { viewModel?.keyDown(keyCode) }
            else { viewModel?.keyUp(keyCode) }
        case 55, 54:
            if flags.contains(.command) { viewModel?.keyDown(keyCode) }
            else { viewModel?.keyUp(keyCode) }
        default:
            break
        }
    }
}

struct LisaDisplayViewRepresentable: NSViewRepresentable {
    let viewModel: EmulatorViewModel

    func makeNSView(context: Context) -> LisaDisplayNSView {
        let view = LisaDisplayNSView()
        view.viewModel = viewModel
        view.wantsLayer = true
        view.layer?.backgroundColor = NSColor.clear.cgColor
        view.layer?.magnificationFilter = .nearest
        return view
    }

    func updateNSView(_ nsView: LisaDisplayNSView, context: Context) {
        nsView.viewModel = viewModel
    }
}

struct LisaDisplayView: View {
    var viewModel: EmulatorViewModel

    var body: some View {
        ZStack {
            Color.black

            if let image = viewModel.displayImage {
                Image(decorative: image, scale: 1.0)
                    .interpolation(.none)
                    .resizable()
                    .aspectRatio(
                        CGFloat(viewModel.screenWidth) / CGFloat(viewModel.screenHeight),
                        contentMode: .fit
                    )
            } else {
                VStack(spacing: 16) {
                    Image(systemName: "desktopcomputer")
                        .font(.system(size: 64))
                        .foregroundStyle(.gray)

                    Text("Apple Lisa")
                        .font(.system(size: 28, weight: .light, design: .serif))
                        .foregroundStyle(.gray)

                    Text(viewModel.statusMessage)
                        .font(.system(size: 14, design: .monospaced))
                        .foregroundStyle(.gray.opacity(0.7))
                }
            }
        }
        .overlay {
            if viewModel.isRunning {
                LisaDisplayViewRepresentable(viewModel: viewModel)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
    }
}
