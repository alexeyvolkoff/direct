import UIKit
import UserNotifications
import Capacitor

@UIApplicationMain
class AppDelegate: UIResponder, UIApplicationDelegate {

    var window: UIWindow?

    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        registerOmdNotificationCategories()
        return true
    }

    private func registerOmdNotificationCategories() {
        let approveAction = UNNotificationAction(
            identifier: "approve",
            title: "✅ В расписание",
            options: [.foreground]
        )
        let reworkAction = UNNotificationAction(
            identifier: "rework",
            title: "🔄 Доработать",
            options: [.foreground]
        )
        let declineAction = UNNotificationAction(
            identifier: "decline",
            title: "❌ Отклонить",
            options: [.destructive, .foreground]
        )
        let moderationCategory = UNNotificationCategory(
            identifier: "OMD_MODERATION",
            actions: [approveAction, reworkAction, declineAction],
            intentIdentifiers: [],
            options: []
        )
        UNUserNotificationCenter.current().setNotificationCategories([moderationCategory])
    }

    func application(_ application: UIApplication, didRegisterForRemoteNotificationsWithDeviceToken deviceToken: Data) {
        NotificationCenter.default.post(name: .capacitorDidRegisterForRemoteNotifications, object: deviceToken)
    }

    func application(_ application: UIApplication, didFailToRegisterForRemoteNotificationsWithError error: Error) {
        NotificationCenter.default.post(name: .capacitorDidFailToRegisterForRemoteNotifications, object: error)
    }

    func applicationWillResignActive(_ application: UIApplication) {
    }

    func applicationDidEnterBackground(_ application: UIApplication) {
    }

    func applicationWillEnterForeground(_ application: UIApplication) {
    }

    func applicationDidBecomeActive(_ application: UIApplication) {
    }

    func applicationWillTerminate(_ application: UIApplication) {
    }

    func application(_ app: UIApplication, open url: URL, options: [UIApplication.OpenURLOptionsKey: Any] = [:]) -> Bool {
        forwardDeepLinkToWebView(url)
        return ApplicationDelegateProxy.shared.application(app, open: url, options: options)
    }

    private func forwardDeepLinkToWebView(_ url: URL) {
        let raw = url.absoluteString
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "'", with: "\\'")
        let js = "window.__pendingDeepLinkUrl='\(raw)';window.dispatchEvent(new CustomEvent('omd-deeplink',{detail:{url:'\(raw)'}}));"
        DispatchQueue.main.async { [weak self] in
            if let vc = self?.window?.rootViewController as? CAPBridgeViewController {
                vc.webView?.evaluateJavaScript(js, completionHandler: nil)
            }
        }
    }

    func application(_ application: UIApplication, continue userActivity: NSUserActivity, restorationHandler: @escaping ([UIUserActivityRestoring]?) -> Void) -> Bool {
        return ApplicationDelegateProxy.shared.application(application, continue: userActivity, restorationHandler: restorationHandler)
    }

}
