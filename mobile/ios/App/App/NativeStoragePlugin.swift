import Foundation
import UIKit
import Photos
import Vision
import CoreLocation
import ImageIO
import AVFoundation
import UniformTypeIdentifiers
import Capacitor

@objc(NativeStoragePlugin)
public class NativeStoragePlugin: CAPPlugin, CAPBridgedPlugin {
    public let identifier = "NativeStoragePlugin"
    public let jsName = "NativeStorage"
    public let pluginMethods: [CAPPluginMethod] = [
        CAPPluginMethod(name: "listRoots", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "list", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "stat", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "readChunk", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "writeChunk", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "mkdir", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "delete", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "getThumbnail", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "analyzeImage", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "listMedia", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "clearCache", returnType: CAPPluginReturnPromise),
        CAPPluginMethod(name: "getAppVersion", returnType: CAPPluginReturnPromise)
    ]

    private let workQueue = DispatchQueue(label: "net.onmydisk.nativestorage.work", qos: .userInitiated)
    private let mapLock = NSLock()
    // Virtual path ("/Photos/<name>") -> PHAsset.localIdentifier
    private var pathToAssetId: [String: String] = [:]
    private var assetIdToPath: [String: String] = [:]
    private var geocodeCache: [String: (location: String, poi: String)] = [:]
    private let thumbCache = NSCache<NSString, NSData>()

    // Curated noise labels (clothing, furniture, generic sky/buildings, colors, body parts)
    // Keeps pets (cat, dog, kitten, puppy), events, landmarks, food, nature highlights.
    private static let noiseLabels: Set<String> = [
        "shirt", "t-shirt", "dress shirt", "polo shirt", "sweater", "sweatshirt", "hoodie",
        "jacket", "coat", "blazer", "suit", "tuxedo", "vest", "cardigan", "blouse", "top",
        "dress", "gown", "skirt", "pants", "trousers", "jeans", "denim", "shorts", "leggings",
        "tie", "necktie", "bow tie", "scarf", "belt", "glove", "sock", "shoe", "footwear",
        "sneakers", "boot", "sandal", "slipper", "high heels", "hat", "cap", "baseball cap",
        "beanie", "sun hat", "helmet", "glasses", "eyeglasses", "eyewear", "sunglasses",
        "goggles", "vision care", "watch", "wristwatch", "jewelry", "necklace", "bracelet",
        "earring", "ring", "bag", "handbag", "purse", "backpack", "luggage", "suitcase",
        "umbrella", "clothing", "clothes", "apparel", "outerwear", "sportswear", "uniform",
        "fashion", "fashion design", "fashion accessory", "fashion model", "textile", "fabric",
        "cotton", "linen", "silk", "wool", "leather", "fur", "velvet", "lace", "plaid", "pattern",
        "chair", "armchair", "office chair", "stool", "bench", "table", "desk", "coffee table",
        "dining table", "couch", "sofa", "bed", "mattress", "pillow", "cushion", "blanket",
        "bedding", "cabinet", "cupboard", "wardrobe", "closet", "drawer", "shelf", "shelving",
        "bookcase", "bookshelf", "countertop", "curtain", "window blind", "window treatment",
        "carpet", "rug", "floor", "flooring", "wood flooring", "hardwood", "tile", "ceiling",
        "wall", "wallpaper", "door", "window", "mirror", "stairs", "handrail", "balcony",
        "porch", "patio", "room", "living room", "bedroom", "bathroom", "kitchen", "dining room",
        "office", "interior", "interior design", "furniture", "house", "home", "apartment",
        "property", "real estate", "appliance", "home appliance", "kitchen appliance",
        "lamp", "lampshade", "light fixture", "lighting", "chandelier", "daylighting",
        "plate", "platter", "dishware", "tableware", "serveware", "porcelain", "ceramic",
        "bowl", "cup", "mug", "teacup", "coffee cup", "saucer", "glassware", "stemware",
        "wine glass", "beer glass", "drinking glass", "bottle", "water bottle", "plastic bottle",
        "can", "tin can", "jar", "pitcher", "kettle", "cutlery", "flatware", "fork", "spoon",
        "knife", "napkin", "tablecloth", "tray", "box", "cardboard", "paper", "cord", "cable", "wire",
        "sky", "blue_sky", "cloud", "clouds", "cumulus", "daytime", "atmosphere", "light", "sunlight", "shadow",
        "shade", "reflection", "horizon", "building", "structure", "infrastructure",
        "architecture", "urban area", "metropolitan area", "city", "town", "village",
        "neighbourhood", "residential area", "commercial building", "mixed-use", "office building",
        "skyscraper", "tower block", "facade", "roof", "brick", "concrete", "cement", "asphalt",
        "road", "road surface", "thoroughfare", "highway", "street", "alley", "sidewalk",
        "pavement", "walkway", "curb", "parking", "parking lot", "street light", "traffic light",
        "sign", "signage", "traffic sign", "pole", "utility pole", "electricity", "fence",
        "wire fencing", "gate", "metal", "steel", "iron", "aluminum", "wood", "lumber", "plank",
        "plywood", "glass", "plastic", "rubber", "stone", "rock", "bedrock", "pebble", "gravel",
        "soil", "dirt", "ground", "grass", "lawn", "grassland", "plant", "terrestrial plant",
        "vascular plant", "woody plant", "herbaceous plant", "houseplant", "flowerpot", "shrub",
        "bush", "hedge", "groundcover", "tree", "trunk", "bark", "branch", "twig", "leaf",
        "nature", "natural environment", "natural landscape", "landscape", "ecoregion", "biome",
        "outdoor", "indoor", "foliage", "greenery", "material", "surface", "texture",
        "vehicle", "land vehicle", "motor vehicle", "car", "automobile", "sedan", "suv", "van",
        "truck", "bus", "automotive design", "automotive exterior", "automotive lighting",
        "automotive tire", "automotive wheel system", "headlamp", "bumper", "grille", "hood",
        "fender", "windshield", "car door", "wheel", "rim", "alloy wheel", "tire", "tread",
        "license plate", "vehicle registration plate", "traffic", "transport", "mode of transport",
        "person", "people", "human", "human body", "man", "woman", "boy", "girl", "adult", "youth",
        "child", "toddler", "baby", "face", "facial expression", "head", "forehead", "temple",
        "eyebrow", "eyelash", "eye", "nose", "cheek", "ear", "mouth", "lip", "tooth", "chin",
        "jaw", "neck", "throat", "shoulder", "arm", "elbow", "wrist", "hand", "finger", "thumb",
        "nail", "chest", "torso", "abdomen", "waist", "back", "hip", "leg", "thigh", "knee",
        "ankle", "foot", "toe", "skin", "flesh", "muscle", "joint", "hair", "hairstyle", "bangs",
        "blond", "brown hair", "black hair", "long hair", "facial hair", "beard", "moustache",
        "smile", "laugh", "laughter", "happy", "happiness", "joy", "fun", "leisure", "recreation",
        "cool", "pose", "gesture", "standing", "sitting", "walking", "talking", "conversation",
        "interaction", "sharing", "crowd", "audience", "event", "public event", "social group",
        "selfie", "snapshot", "photography", "photograph", "photo", "picture", "image",
        "stock photography", "portrait", "portrait photography", "close-up", "macro photography",
        "flash photography", "monochrome", "black-and-white", "color", "tint", "magenta",
        "purple", "blue", "electric blue", "azure", "green", "yellow", "orange", "red", "pink",
        "brown", "grey", "gray", "black", "white", "darkness", "font", "text", "symbol", "logo",
        "brand", "advertising", "poster", "document", "handwriting", "line", "rectangle", "square",
        "circle", "triangle", "parallel", "symmetry", "slope", "space", "gadget", "electronic device",
        "electronics", "technology", "display device", "flat panel display", "projection screen",
        "screen", "computer monitor", "television", "laptop", "personal computer", "computer",
        "computer keyboard", "mouse", "mobile phone", "smartphone", "telephony",
        "portable communications device", "communication device", "multimedia", "software", "website",
        "animal", "terrestrial animal", "working animal", "pet", "mammal", "vertebrate",
        "carnivore", "whiskers", "snout", "paw", "claw", "tail", "adult_person", "human_face"
    ]

    public override func load() {
        thumbCache.countLimit = 120
        thumbCache.totalCostLimit = 24 * 1024 * 1024
    }

    private func documentsBaseURL() -> URL {
        return FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSTemporaryDirectory())
    }

    private func tempExportDirURL() -> URL {
        let dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("omd_ios_export", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    private func normalizePath(_ raw: String) -> String {
        var p = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if p.isEmpty { return "/" }
        if !p.hasPrefix("/") { p = "/" + p }
        while p.count > 1 && p.hasSuffix("/") {
            p.removeLast()
        }
        return p
    }

    private func isPhotosVirtualPath(_ path: String) -> Bool {
        let p = normalizePath(path)
        return p == "/Photos" || p.hasPrefix("/Photos/") || p == "/DCIM" || p.hasPrefix("/DCIM/") || p == "/Pictures" || p.hasPrefix("/Pictures/")
    }

    private func resolveDocumentsURL(_ virtualPath: String) -> URL {
        let p = normalizePath(virtualPath)
        var rel = p
        if rel == "/Documents" {
            rel = ""
        } else if rel.hasPrefix("/Documents/") {
            rel = String(rel.dropFirst("/Documents/".count))
        } else {
            rel = String(rel.dropFirst())
        }
        if rel.isEmpty { return documentsBaseURL() }
        return documentsBaseURL().appendingPathComponent(rel)
    }

    private func ensurePhotoPermission(_ completion: @escaping (Bool) -> Void) {
        let status = PHPhotoLibrary.authorizationStatus(for: .readWrite)
        if status == .authorized || status == .limited {
            completion(true)
            return
        }
        if status == .notDetermined {
            PHPhotoLibrary.requestAuthorization(for: .readWrite) { newStatus in
                completion(newStatus == .authorized || newStatus == .limited)
            }
            return
        }
        completion(false)
    }

    private func makeUniquePhotoPath(asset: PHAsset) -> (path: String, name: String, mime: String, size: Int64) {
        mapLock.lock()
        if let existing = assetIdToPath[asset.localIdentifier] {
            mapLock.unlock()
            let nm = (existing as NSString).lastPathComponent
            let isVid = asset.mediaType == .video
            let mime = mimeTypeForName(nm, fallback: isVid ? "video/mp4" : "image/jpeg")
            return (existing, nm, mime, 0)
        }
        mapLock.unlock()

        let resources = PHAssetResource.assetResources(for: asset)
        let primary = resources.first(where: { $0.type == .photo || $0.type == .video || $0.type == .fullSizePhoto || $0.type == .fullSizeVideo }) ?? resources.first
        var origName = primary?.originalFilename ?? ""
        if origName.isEmpty {
            let shortId = asset.localIdentifier.components(separatedBy: "/").first ?? UUID().uuidString.prefix(8).description
            origName = asset.mediaType == .video ? "VID_\(shortId).mov" : "IMG_\(shortId).jpg"
        }
        // Sanitize filename
        origName = origName.replacingOccurrences(of: "/", with: "_")
        var size: Int64 = 0
        if let p = primary, let unsignedSize = p.value(forKey: "fileSize") as? CLong {
            size = Int64(unsignedSize)
        }

        let shortHash = String(asset.localIdentifier.components(separatedBy: "/").first?.prefix(6) ?? "")
        var candidatePath = "/Photos/\(origName)"

        mapLock.lock()
        if let ownerId = pathToAssetId[candidatePath], ownerId != asset.localIdentifier {
            let ext = (origName as NSString).pathExtension
            let base = (origName as NSString).deletingPathExtension
            let uniqueName = ext.isEmpty ? "\(base)_\(shortHash)" : "\(base)_\(shortHash).\(ext)"
            candidatePath = "/Photos/\(uniqueName)"
            origName = uniqueName
        }
        pathToAssetId[candidatePath] = asset.localIdentifier
        assetIdToPath[asset.localIdentifier] = candidatePath
        mapLock.unlock()

        let mime = mimeTypeForName(origName, fallback: asset.mediaType == .video ? "video/mp4" : "image/jpeg")
        return (candidatePath, origName, mime, size)
    }

    private func findAsset(forVirtualPath path: String) -> PHAsset? {
        let norm = normalizePath(path)
        mapLock.lock()
        let cachedId = pathToAssetId[norm]
        mapLock.unlock()
        if let id = cachedId {
            let res = PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil)
            if let first = res.firstObject {
                return first
            }
        }
        // Rebuild index if not cached yet (e.g. cold start with persisted path)
        let opts = PHFetchOptions()
        opts.sortDescriptors = [NSSortDescriptor(key: "creationDate", ascending: false)]
        let all = PHAsset.fetchAssets(with: opts)
        var matched: PHAsset? = nil
        all.enumerateObjects { asset, _, stop in
            let info = self.makeUniquePhotoPath(asset: asset)
            if info.path == norm {
                matched = asset
                stop.pointee = true
            }
        }
        return matched
    }

    private func mimeTypeForName(_ name: String, fallback: String = "application/octet-stream") -> String {
        let ext = (name as NSString).pathExtension.lowercased()
        if ext.isEmpty { return fallback }
        if let ut = UTType(filenameExtension: ext), let mime = ut.preferredMIMEType {
            return mime
        }
        switch ext {
        case "jpg", "jpeg": return "image/jpeg"
        case "png": return "image/png"
        case "heic", "heif": return "image/heic"
        case "webp": return "image/webp"
        case "gif": return "image/gif"
        case "mp4", "m4v": return "video/mp4"
        case "mov": return "video/quicktime"
        case "pdf": return "application/pdf"
        case "md": return "text/markdown"
        case "txt": return "text/plain"
        case "json": return "application/json"
        default: return fallback
        }
    }

    private func exportAssetToTempFile(_ asset: PHAsset, completion: @escaping (URL?) -> Void) {
        let safeId = asset.localIdentifier.replacingOccurrences(of: "/", with: "_")
        let resources = PHAssetResource.assetResources(for: asset)
        let primary = resources.first(where: { $0.type == .photo || $0.type == .video || $0.type == .fullSizePhoto || $0.type == .fullSizeVideo }) ?? resources.first
        let ext = (primary?.originalFilename as NSString?)?.pathExtension ?? (asset.mediaType == .video ? "mov" : "jpg")
        let outURL = tempExportDirURL().appendingPathComponent("\(safeId).\(ext.isEmpty ? "dat" : ext)")
        if FileManager.default.fileExists(atPath: outURL.path) {
            completion(outURL)
            return
        }
        guard let res = primary else {
            completion(nil)
            return
        }
        let opts = PHAssetResourceRequestOptions()
        opts.isNetworkAccessAllowed = true
        PHAssetResourceManager.default().writeData(for: res, toFile: outURL, options: opts) { err in
            if err != nil {
                try? FileManager.default.removeItem(at: outURL)
                completion(nil)
            } else {
                completion(outURL)
            }
        }
    }

    // MARK: - Plugin Methods

    @objc func listRoots(_ call: CAPPluginCall) {
        let nowMs = Int64(Date().timeIntervalSince1970 * 1000)
        let roots: [[String: Any]] = [
            ["id": "/Photos", "name": "Photos", "isDir": true, "date": nowMs],
            ["id": "/Documents", "name": "Documents", "isDir": true, "date": nowMs]
        ]
        call.resolve(["roots": roots])
    }

    @objc func list(_ call: CAPPluginCall) {
        let path = normalizePath(call.getString("path") ?? "/")
        if path == "/" {
            listRoots(call)
            return
        }
        if isPhotosVirtualPath(path) {
            ensurePhotoPermission { granted in
                if !granted {
                    call.resolve(["files": []])
                    return
                }
                self.workQueue.async {
                    let opts = PHFetchOptions()
                    opts.sortDescriptors = [NSSortDescriptor(key: "creationDate", ascending: false)]
                    opts.fetchLimit = 2000
                    let assets = PHAsset.fetchAssets(with: opts)
                    var files: [[String: Any]] = []
                    assets.enumerateObjects { asset, _, _ in
                        if asset.mediaType != .image && asset.mediaType != .video { return }
                        let info = self.makeUniquePhotoPath(asset: asset)
                        let dtMs = Int64((asset.creationDate ?? asset.modificationDate ?? Date()).timeIntervalSince1970 * 1000)
                        files.append([
                            "id": info.path,
                            "name": info.name,
                            "type": info.mime,
                            "size": info.size,
                            "date": dtMs,
                            "isDir": false
                        ])
                    }
                    call.resolve(["files": files])
                }
            }
            return
        }

        workQueue.async {
            let dirURL = self.resolveDocumentsURL(path)
            var files: [[String: Any]] = []
            if let entries = try? FileManager.default.contentsOfDirectory(at: dirURL, includingPropertiesForKeys: [.isDirectoryKey, .fileSizeKey, .contentModificationDateKey], options: [.skipsHiddenFiles]) {
                for url in entries {
                    let vals = try? url.resourceValues(forKeys: [.isDirectoryKey, .fileSizeKey, .contentModificationDateKey])
                    let isDir = vals?.isDirectory ?? false
                    let size = Int64(vals?.fileSize ?? 0)
                    let dtMs = Int64((vals?.contentModificationDate ?? Date()).timeIntervalSince1970 * 1000)
                    let name = url.lastPathComponent
                    let childId = path == "/" ? "/\(name)" : "\(path)/\(name)"
                    files.append([
                        "id": childId,
                        "name": name,
                        "type": isDir ? "folder" : self.mimeTypeForName(name),
                        "size": size,
                        "date": dtMs,
                        "isDir": isDir
                    ])
                }
            }
            call.resolve(["files": files])
        }
    }

    @objc func listMedia(_ call: CAPPluginCall) {
        let limit = call.getInt("limit") ?? 5000
        let offset = call.getInt("offset") ?? 0
        ensurePhotoPermission { granted in
            if !granted {
                call.resolve(["items": [], "count": 0])
                return
            }
            self.workQueue.async {
                let opts = PHFetchOptions()
                opts.sortDescriptors = [NSSortDescriptor(key: "creationDate", ascending: false)]
                let assets = PHAsset.fetchAssets(with: opts)
                var items: [[String: Any]] = []
                let total = assets.count
                if offset < total {
                    let end = min(total, offset + limit)
                    for idx in offset..<end {
                        let asset = assets.object(at: idx)
                        if asset.mediaType != .image && asset.mediaType != .video { continue }
                        let info = self.makeUniquePhotoPath(asset: asset)
                        let dtMs = Int64((asset.creationDate ?? asset.modificationDate ?? Date()).timeIntervalSince1970 * 1000)
                        items.append([
                            "id": info.path,
                            "realPath": info.path,
                            "name": info.name,
                            "type": info.mime,
                            "size": info.size,
                            "date": dtMs,
                            "isDir": false
                        ])
                    }
                }
                call.resolve(["items": items, "count": items.count])
            }
        }
    }

    @objc func stat(_ call: CAPPluginCall) {
        let path = normalizePath(call.getString("path") ?? "")
        if path == "/" || path == "/Photos" || path == "/Documents" {
            call.resolve([
                "exists": true,
                "name": (path as NSString).lastPathComponent,
                "isDir": true,
                "size": 0,
                "type": "folder"
            ])
            return
        }
        if isPhotosVirtualPath(path) {
            workQueue.async {
                guard let asset = self.findAsset(forVirtualPath: path) else {
                    call.resolve(["exists": false])
                    return
                }
                let info = self.makeUniquePhotoPath(asset: asset)
                call.resolve([
                    "exists": true,
                    "name": info.name,
                    "isDir": false,
                    "size": info.size,
                    "type": info.mime
                ])
            }
            return
        }
        workQueue.async {
            let url = self.resolveDocumentsURL(path)
            var isDirObj: ObjCBool = false
            let exists = FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirObj)
            if !exists {
                call.resolve(["exists": false])
                return
            }
            let attrs = try? FileManager.default.attributesOfItem(atPath: url.path)
            let size = (attrs?[.size] as? NSNumber)?.int64Value ?? 0
            call.resolve([
                "exists": true,
                "name": url.lastPathComponent,
                "isDir": isDirObj.boolValue,
                "size": size,
                "type": isDirObj.boolValue ? "folder" : self.mimeTypeForName(url.lastPathComponent)
            ])
        }
    }

    @objc func readChunk(_ call: CAPPluginCall) {
        let path = normalizePath(call.getString("path") ?? "")
        let offset = Int64(call.getInt("offset") ?? 0)
        let length = call.getInt("length") ?? 65536

        let readFileURL: (URL) -> Void = { fileURL in
            do {
                let attrs = try FileManager.default.attributesOfItem(atPath: fileURL.path)
                let totalSize = (attrs[.size] as? NSNumber)?.int64Value ?? 0
                if offset >= totalSize {
                    call.resolve(["data": "", "bytesRead": 0, "totalSize": totalSize, "eof": true])
                    return
                }
                let fh = try FileHandle(forReadingFrom: fileURL)
                defer { try? fh.close() }
                try fh.seek(toOffset: UInt64(max(0, offset)))
                let data = fh.readData(ofLength: max(1, length))
                let eof = (offset + Int64(data.count)) >= totalSize
                call.resolve([
                    "data": data.base64EncodedString(),
                    "bytesRead": data.count,
                    "totalSize": totalSize,
                    "eof": eof
                ])
            } catch {
                call.reject("readChunk failed: \(error.localizedDescription)")
            }
        }

        if isPhotosVirtualPath(path) {
            workQueue.async {
                guard let asset = self.findAsset(forVirtualPath: path) else {
                    call.reject("Photo asset not found: \(path)")
                    return
                }
                self.exportAssetToTempFile(asset) { tempURL in
                    guard let url = tempURL else {
                        call.reject("Failed to export photo asset: \(path)")
                        return
                    }
                    readFileURL(url)
                }
            }
            return
        }

        workQueue.async {
            let url = self.resolveDocumentsURL(path)
            readFileURL(url)
        }
    }

    @objc func writeChunk(_ call: CAPPluginCall) {
        let path = normalizePath(call.getString("path") ?? "")
        let b64 = call.getString("data") ?? ""
        let append = call.getBool("append") ?? false
        guard let data = Data(base64Encoded: b64) else {
            call.reject("Invalid base64 data")
            return
        }
        workQueue.async {
            let url = self.resolveDocumentsURL(path)
            do {
                try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
                if append && FileManager.default.fileExists(atPath: url.path) {
                    let fh = try FileHandle(forWritingTo: url)
                    defer { try? fh.close() }
                    try fh.seekToEnd()
                    fh.write(data)
                } else {
                    try data.write(to: url, options: .atomic)
                }
                call.resolve(["bytesWritten": data.count, "path": path])
            } catch {
                call.reject("writeChunk failed: \(error.localizedDescription)")
            }
        }
    }

    @objc func mkdir(_ call: CAPPluginCall) {
        let path = normalizePath(call.getString("path") ?? "")
        if path.isEmpty || path == "/" {
            call.reject("Path is required")
            return
        }
        workQueue.async {
            let url = self.resolveDocumentsURL(path)
            do {
                try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
                call.resolve(["success": true, "path": path])
            } catch {
                call.reject("mkdir failed: \(error.localizedDescription)")
            }
        }
    }

    @objc func delete(_ call: CAPPluginCall) {
        let path = normalizePath(call.getString("path") ?? "")
        if path.isEmpty || isPhotosVirtualPath(path) {
            call.reject("Cannot delete read-only path: \(path)")
            return
        }
        workQueue.async {
            let url = self.resolveDocumentsURL(path)
            do {
                if FileManager.default.fileExists(atPath: url.path) {
                    try FileManager.default.removeItem(at: url)
                }
                call.resolve(["success": true, "path": path])
            } catch {
                call.reject("delete failed: \(error.localizedDescription)")
            }
        }
    }

    @objc func getThumbnail(_ call: CAPPluginCall) {
        let path = normalizePath(call.getString("path") ?? "")
        let width = max(64, call.getInt("width") ?? 360)
        let height = max(64, call.getInt("height") ?? 360)
        let cacheKey = "\(path)|\(width)x\(height)" as NSString

        if let cached = thumbCache.object(forKey: cacheKey) {
            call.resolve([
                "data": (cached as Data).base64EncodedString(),
                "mimeType": "image/jpeg"
            ])
            return
        }

        if isPhotosVirtualPath(path) {
            workQueue.async {
                guard let asset = self.findAsset(forVirtualPath: path) else {
                    call.reject("Asset not found: \(path)")
                    return
                }
                let opts = PHImageRequestOptions()
                opts.deliveryMode = .highQualityFormat
                opts.resizeMode = .fast
                opts.isNetworkAccessAllowed = true
                opts.isSynchronous = false
                let targetSize = CGSize(width: width, height: height)
                PHImageManager.default().requestImage(for: asset, targetSize: targetSize, contentMode: .aspectFit, options: opts) { img, _ in
                    guard let image = img, let jpeg = image.jpegData(compressionQuality: 0.82) else {
                        call.reject("Thumbnail failed for: \(path)")
                        return
                    }
                    self.thumbCache.setObject(jpeg as NSData, forKey: cacheKey, cost: jpeg.count)
                    call.resolve([
                        "data": jpeg.base64EncodedString(),
                        "mimeType": "image/jpeg"
                    ])
                }
            }
            return
        }

        workQueue.async {
            let url = self.resolveDocumentsURL(path)
            let mime = self.mimeTypeForName(url.lastPathComponent)
            if mime.hasPrefix("video/") {
                let avAsset = AVURLAsset(url: url)
                let gen = AVAssetImageGenerator(asset: avAsset)
                gen.appliesPreferredTrackTransform = true
                gen.maximumSize = CGSize(width: width, height: height)
                if let cg = try? gen.copyCGImage(at: CMTime(seconds: 0.5, preferredTimescale: 600), actualTime: nil),
                   let jpeg = UIImage(cgImage: cg).jpegData(compressionQuality: 0.82) {
                    self.thumbCache.setObject(jpeg as NSData, forKey: cacheKey, cost: jpeg.count)
                    call.resolve(["data": jpeg.base64EncodedString(), "mimeType": "image/jpeg"])
                    return
                }
                call.reject("Video thumbnail failed: \(path)")
                return
            }

            if let src = CGImageSourceCreateWithURL(url as CFURL, nil) {
                let opts: [CFString: Any] = [
                    kCGImageSourceCreateThumbnailFromImageAlways: true,
                    kCGImageSourceCreateThumbnailWithTransform: true,
                    kCGImageSourceThumbnailMaxPixelSize: max(width, height)
                ]
                if let cg = CGImageSourceCreateThumbnailAtIndex(src, 0, opts as CFDictionary),
                   let jpeg = UIImage(cgImage: cg).jpegData(compressionQuality: 0.82) {
                    self.thumbCache.setObject(jpeg as NSData, forKey: cacheKey, cost: jpeg.count)
                    call.resolve(["data": jpeg.base64EncodedString(), "mimeType": "image/jpeg"])
                    return
                }
            }
            call.reject("Thumbnail failed for: \(path)")
        }
    }

    // MARK: - On-Device Vision + OCR + Face Detection + Reverse Geocoding

    private static func isNativeNoiseLabel(_ raw: String) -> Bool {
        let low = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            .lowercased()
            .replacingOccurrences(of: "_", with: " ")
        if low.isEmpty { return true }
        if noiseLabels.contains(low) || noiseLabels.contains(raw.lowercased()) { return true }
        let blockedSubstrings = [
            "shirt", "jacket", "jeans", "denim", "trousers", "footwear", "eyewear",
            "glasses", "clothing", "fashion", "chair", "tableware", "dishware",
            "furniture", "flooring", "appliance", "automotive", "motor vehicle",
            "road surface", "infrastructure"
        ]
        for sub in blockedSubstrings {
            if low.contains(sub) { return true }
        }
        return false
    }

    private static func prettifyVisionLabel(_ identifier: String) -> String {
        let cleaned = identifier.replacingOccurrences(of: "_", with: " ").trimmingCharacters(in: .whitespacesAndNewlines)
        guard let first = cleaned.first else { return cleaned }
        return first.uppercased() + cleaned.dropFirst()
    }

    private static func isValidPoiCandidate(_ cand: String?, street: String?, city: String?, admin: String?, country: String?) -> Bool {
        guard let raw = cand?.trimmingCharacters(in: .whitespacesAndNewlines) else { return false }
        if raw.count < 4 || raw.count > 80 { return false }
        if raw.range(of: "^\\d+[A-Za-zА-Яа-я0-9/\\-\\s]*$", options: .regularExpression) != nil { return false }
        if raw.range(of: "^[23456789CFGHJMPQRVWX]{4,8}\\+.*", options: .regularExpression) != nil { return false }
        if raw.range(of: "[A-Za-zА-Яа-яІіЇїЄєҐґČčŠšŽžĆćĐđ]", options: .regularExpression) == nil { return false }
        let low = raw.lowercased()
        if let st = street?.trimmingCharacters(in: .whitespacesAndNewlines), !st.isEmpty, low == st.lowercased() { return false }
        if let ci = city?.trimmingCharacters(in: .whitespacesAndNewlines), !ci.isEmpty, low == ci.lowercased() { return false }
        if let ad = admin?.trimmingCharacters(in: .whitespacesAndNewlines), !ad.isEmpty, low == ad.lowercased() { return false }
        if let co = country?.trimmingCharacters(in: .whitespacesAndNewlines), !co.isEmpty, low == co.lowercased() { return false }
        if low.contains("oblast") || low.contains("область") || low.contains("район") || low.contains("raion") { return false }
        return true
    }

    private func reverseGeocode(lat: Double, lon: Double, completion: @escaping (String, String) -> Void) {
        let key = String(format: "%.4f,%.4f", lat, lon)
        mapLock.lock()
        if let cached = geocodeCache[key] {
            mapLock.unlock()
            completion(cached.location, cached.poi)
            return
        }
        mapLock.unlock()

        let loc = CLLocation(latitude: lat, longitude: lon)
        CLGeocoder().reverseGeocodeLocation(loc) { placemarks, _ in
            var resolvedLocation = ""
            var resolvedPoi = ""
            if let pm = placemarks?.first {
                var city = pm.locality ?? pm.subAdministrativeArea ?? ""
                if city.isEmpty, let adm = pm.administrativeArea {
                    let lowAdm = adm.lowercased()
                    if !lowAdm.contains("oblast") && !lowAdm.contains("область") {
                        city = adm
                    }
                }
                let country = pm.country ?? ""
                if !city.isEmpty && !country.isEmpty {
                    resolvedLocation = "\(city), \(country)"
                } else if !city.isEmpty {
                    resolvedLocation = city
                } else if !country.isEmpty {
                    resolvedLocation = country
                }

                if let areas = pm.areasOfInterest {
                    for area in areas {
                        if Self.isValidPoiCandidate(area, street: pm.thoroughfare, city: city, admin: pm.administrativeArea, country: country) {
                            resolvedPoi = area.trimmingCharacters(in: .whitespacesAndNewlines)
                            break
                        }
                    }
                }
                if resolvedPoi.isEmpty, Self.isValidPoiCandidate(pm.name, street: pm.thoroughfare, city: city, admin: pm.administrativeArea, country: country) {
                    resolvedPoi = (pm.name ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
                }
            }
            self.mapLock.lock()
            self.geocodeCache[key] = (resolvedLocation, resolvedPoi)
            self.mapLock.unlock()
            completion(resolvedLocation, resolvedPoi)
        }
    }

    private func extractExifMetadata(from url: URL) -> (lat: Double?, lon: Double?, takenAt: String) {
        var lat: Double? = nil
        var lon: Double? = nil
        var takenAt = ""
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil),
              let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any] else {
            return (nil, nil, "")
        }
        if let exif = props[kCGImagePropertyExifDictionary] as? [CFString: Any] {
            if let dt = exif[kCGImagePropertyExifDateTimeOriginal] as? String, !dt.isEmpty {
                takenAt = dt
            }
        }
        if takenAt.isEmpty, let tiff = props[kCGImagePropertyTIFFDictionary] as? [CFString: Any] {
            if let dt = tiff[kCGImagePropertyTIFFDateTime] as? String, !dt.isEmpty {
                takenAt = dt
            }
        }
        if let gps = props[kCGImagePropertyGPSDictionary] as? [CFString: Any],
           let rawLat = gps[kCGImagePropertyGPSLatitude] as? Double,
           let rawLon = gps[kCGImagePropertyGPSLongitude] as? Double {
            let latRef = (gps[kCGImagePropertyGPSLatitudeRef] as? String) ?? "N"
            let lonRef = (gps[kCGImagePropertyGPSLongitudeRef] as? String) ?? "E"
            let signedLat = latRef.uppercased() == "S" ? -abs(rawLat) : abs(rawLat)
            let signedLon = lonRef.uppercased() == "W" ? -abs(rawLon) : abs(rawLon)
            if signedLat != 0.0 || signedLon != 0.0 {
                lat = signedLat
                lon = signedLon
            }
        }
        return (lat, lon, takenAt)
    }

    private func runVisionPipeline(
        cgImage: CGImage,
        gpsLat: Double?,
        gpsLon: Double?,
        takenAt: String,
        t0: CFAbsoluteTime,
        call: CAPPluginCall
    ) {
        workQueue.async {
            let classifyReq = VNClassifyImageRequest()
            let textReq = VNRecognizeTextRequest()
            textReq.recognitionLevel = .accurate
            textReq.usesLanguageCorrection = true
            if #available(iOS 16.0, *) {
                textReq.automaticallyDetectsLanguage = true
            }
            let faceReq = VNDetectFaceRectanglesRequest()

            let handler = VNImageRequestHandler(cgImage: cgImage, orientation: .up, options: [:])
            do {
                try handler.perform([classifyReq, textReq, faceReq])
            } catch {
                call.reject("Vision analysis failed: \(error.localizedDescription)")
                return
            }

            // 1. Labels (filtered against clothing/sky/building noise)
            var labels: [String] = []
            var labelDetails: [[String: Any]] = []
            if let obs = classifyReq.results {
                let top = obs.filter { $0.confidence >= 0.35 }.prefix(24)
                for item in top {
                    if Self.isNativeNoiseLabel(item.identifier) { continue }
                    let pretty = Self.prettifyVisionLabel(item.identifier)
                    if labels.contains(pretty) { continue }
                    labels.append(pretty)
                    labelDetails.append([
                        "text": pretty,
                        "confidence": Double(item.confidence)
                    ])
                    if labels.count >= 12 { break }
                }
            }

            // 2. OCR text + prominent headlines sorted by visual height
            var ocrLines: [String] = []
            var headlineCandidates: [(text: String, relH: Float)] = []
            if let textObs = textReq.results {
                for obs in textObs {
                    guard let best = obs.topCandidates(1).first else { continue }
                    let line = best.string.trimmingCharacters(in: .whitespacesAndNewlines)
                    if line.isEmpty { continue }
                    ocrLines.append(line)
                    let relH = Float(obs.boundingBox.height)
                    if line.count >= 3 && line.count <= 80 && relH >= 0.024 &&
                        line.range(of: "[A-Za-zА-Яа-яІіЇїЄєҐґČčŠšŽž]{2,}", options: .regularExpression) != nil {
                        headlineCandidates.append((line, relH))
                    }
                }
            }
            let ocrText = ocrLines.joined(separator: "\n")
            headlineCandidates.sort { $0.relH > $1.relH }
            let headlines: [[String: Any]] = headlineCandidates.prefix(5).map {
                ["text": $0.text, "relHeight": Double($0.relH)]
            }

            // 3. Faces
            let faceCount = faceReq.results?.count ?? 0

            let finish: (String, String) -> Void = { location, poi in
                let elapsedMs = Int((CFAbsoluteTimeGetCurrent() - t0) * 1000.0)
                var ret: [String: Any] = [
                    "labels": labels,
                    "labelDetails": labelDetails,
                    "text": ocrText,
                    "headlines": headlines,
                    "faceCount": faceCount,
                    "location": location,
                    "poi": poi,
                    "elapsedMs": elapsedMs
                ]
                if let lat = gpsLat, let lon = gpsLon {
                    ret["gpsLat"] = lat
                    ret["gpsLon"] = lon
                }
                if !takenAt.isEmpty {
                    ret["takenAt"] = takenAt
                }
                call.resolve(ret)
            }

            if let lat = gpsLat, let lon = gpsLon {
                self.reverseGeocode(lat: lat, lon: lon) { locStr, poiStr in
                    finish(locStr, poiStr)
                }
            } else {
                finish("", "")
            }
        }
    }

    @objc func analyzeImage(_ call: CAPPluginCall) {
        let path = normalizePath(call.getString("path") ?? "")
        let maxDim = max(256, call.getInt("maxDim") ?? 768)
        if path.isEmpty || path == "/" {
            call.reject("Path is required")
            return
        }
        let t0 = CFAbsoluteTimeGetCurrent()

        if isPhotosVirtualPath(path) {
            workQueue.async {
                guard let asset = self.findAsset(forVirtualPath: path) else {
                    call.reject("Photo asset not found: \(path)")
                    return
                }
                var gpsLat: Double? = nil
                var gpsLon: Double? = nil
                if let loc = asset.location {
                    let c = loc.coordinate
                    if c.latitude != 0.0 || c.longitude != 0.0 {
                        gpsLat = c.latitude
                        gpsLon = c.longitude
                    }
                }
                var takenAt = ""
                if let dt = asset.creationDate {
                    let df = DateFormatter()
                    df.locale = Locale(identifier: "en_US_POSIX")
                    df.dateFormat = "yyyy:MM:dd HH:mm:ss"
                    takenAt = df.string(from: dt)
                }

                let opts = PHImageRequestOptions()
                opts.deliveryMode = .highQualityFormat
                opts.resizeMode = .exact
                opts.isNetworkAccessAllowed = true
                opts.isSynchronous = false
                let targetSize = CGSize(width: maxDim, height: maxDim)
                PHImageManager.default().requestImage(for: asset, targetSize: targetSize, contentMode: .aspectFit, options: opts) { img, _ in
                    guard let uiImg = img else {
                        call.reject("Failed to load image for analysis: \(path)")
                        return
                    }
                    // Render upright CGImage so Vision orientation matches UI
                    let renderer = UIGraphicsImageRenderer(size: uiImg.size)
                    let upright = renderer.image { _ in
                        uiImg.draw(in: CGRect(origin: .zero, size: uiImg.size))
                    }
                    guard let cg = upright.cgImage else {
                        call.reject("Failed to decode CGImage: \(path)")
                        return
                    }
                    self.runVisionPipeline(cgImage: cg, gpsLat: gpsLat, gpsLon: gpsLon, takenAt: takenAt, t0: t0, call: call)
                }
            }
            return
        }

        workQueue.async {
            let url = self.resolveDocumentsURL(path)
            guard FileManager.default.fileExists(atPath: url.path) else {
                call.reject("File not found: \(path)")
                return
            }
            let meta = self.extractExifMetadata(from: url)
            guard let src = CGImageSourceCreateWithURL(url as CFURL, nil) else {
                call.reject("Failed to open image: \(path)")
                return
            }
            let opts: [CFString: Any] = [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: maxDim
            ]
            guard let cg = CGImageSourceCreateThumbnailAtIndex(src, 0, opts as CFDictionary) else {
                call.reject("Failed to decode image: \(path)")
                return
            }
            self.runVisionPipeline(cgImage: cg, gpsLat: meta.lat, gpsLon: meta.lon, takenAt: meta.takenAt, t0: t0, call: call)
        }
    }

    @objc func clearCache(_ call: CAPPluginCall) {
        thumbCache.removeAllObjects()
        try? FileManager.default.removeItem(at: tempExportDirURL())
        call.resolve(["cleared": true])
    }

    @objc func getAppVersion(_ call: CAPPluginCall) {
        let info = Bundle.main.infoDictionary
        let version = info?["CFBundleShortVersionString"] as? String ?? "1.0.0"
        let buildStr = info?["CFBundleVersion"] as? String ?? "1"
        call.resolve([
            "version": version,
            "build": Int(buildStr) ?? 1
        ])
    }
}
