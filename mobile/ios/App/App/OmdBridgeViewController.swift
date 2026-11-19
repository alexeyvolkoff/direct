import UIKit
import Capacitor

class OmdBridgeViewController: CAPBridgeViewController {
    override open func capacitorDidLoad() {
        bridge?.registerPluginInstance(NativeStoragePlugin())
    }
}
