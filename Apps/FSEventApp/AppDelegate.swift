import Cocoa

@main
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        LoginItemController.shared.syncLoginItemRegistration()
    }

    func applicationWillTerminate(_ notification: Notification) {
        // Additional teardown hooks for the GUI can live here.
    }
}
