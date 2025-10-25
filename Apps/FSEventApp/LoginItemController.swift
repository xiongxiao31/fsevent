import Foundation
import ServiceManagement

final class LoginItemController {
    static let shared = LoginItemController()

    private let helperIdentifier = "com.example.fsevent.helper"
    private let loginItemPreferenceKey = "loginItemEnabled"

    func syncLoginItemRegistration() {
        do {
            let service = try SMAppService.loginItem(identifier: helperIdentifier)
            let wantsLoginItem = UserDefaults.standard.bool(forKey: loginItemPreferenceKey)
            if wantsLoginItem {
                if !service.status.contains(.enabled) {
                    try service.register()
                }
            } else {
                if service.status.contains(.enabled) {
                    try service.unregister()
                }
            }
        } catch {
            NSLog("Failed to sync login item registration: %@", error.localizedDescription)
        }
    }

    func setLoginItemEnabled(_ enabled: Bool) {
        UserDefaults.standard.set(enabled, forKey: loginItemPreferenceKey)
        syncLoginItemRegistration()
    }
}
