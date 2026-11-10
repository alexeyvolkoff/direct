package net.onmydisk.app;

import android.Manifest;
import android.content.BroadcastReceiver;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.MediaMetadataRetriever;
import android.media.ThumbnailUtils;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.MediaStore;
import android.util.Base64;
import android.util.Log;
import android.util.LruCache;
import android.util.Size;
import android.webkit.MimeTypeMap;

import com.getcapacitor.PermissionState;
import com.getcapacitor.annotation.PermissionCallback;
import com.getcapacitor.JSArray;
import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;
import com.getcapacitor.annotation.Permission;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

@CapacitorPlugin(
    name = "NativeStorage",
    permissions = {
        @Permission(
            alias = "storage",
            strings = {
                Manifest.permission.READ_EXTERNAL_STORAGE,
                Manifest.permission.WRITE_EXTERNAL_STORAGE
            }
        ),
        @Permission(
            alias = "media",
            strings = {
                "android.permission.READ_MEDIA_IMAGES",
                "android.permission.READ_MEDIA_VIDEO",
                "android.permission.READ_MEDIA_AUDIO"
            }
        )
    }
)
public class NativeStoragePlugin extends Plugin {
    private static final String TAG = "NativeStoragePlugin";

    // Virtual root aliases
    private static final String ROOT_DCIM = "DCIM";
    private static final String ROOT_PICTURES = "Pictures";
    private static final String ROOT_DOCUMENTS = "Documents";
    private static final String ROOT_DOWNLOAD = "Download";
    private static final String ROOT_APP_DATA = "AppData";

    private final LruCache<String, JSObject> thumbnailCache = new LruCache<>(300);
    private final java.util.concurrent.Semaphore thumbnailSemaphore = new java.util.concurrent.Semaphore(2);

    private BroadcastReceiver mediaReceiver;
    private Object volumeCallback;
    private android.database.ContentObserver mediaObserver;

    @Override
    public void load() {
        super.load();
        registerStorageListeners();
    }

    private void registerStorageListeners() {
        try {
            IntentFilter filter = new IntentFilter();
            filter.addAction(Intent.ACTION_MEDIA_MOUNTED);
            filter.addAction(Intent.ACTION_MEDIA_UNMOUNTED);
            filter.addAction(Intent.ACTION_MEDIA_REMOVED);
            filter.addAction(Intent.ACTION_MEDIA_EJECT);
            filter.addAction(Intent.ACTION_MEDIA_BAD_REMOVAL);
            filter.addDataScheme("file");

            mediaReceiver = new BroadcastReceiver() {
                @Override
                public void onReceive(Context context, Intent intent) {
                    String action = intent != null ? intent.getAction() : "";
                    Log.i(TAG, "Media broadcast received: " + action);
                    notifyStorageChanged(action);
                }
            };

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                getContext().registerReceiver(mediaReceiver, filter, Context.RECEIVER_EXPORTED);
            } else {
                getContext().registerReceiver(mediaReceiver, filter);
            }
        } catch (Exception e) {
            Log.w(TAG, "Failed to register media receiver: ", e);
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                StorageManager sm = (StorageManager) getContext().getSystemService(Context.STORAGE_SERVICE);
                if (sm != null) {
                    StorageManager.StorageVolumeCallback cb = new StorageManager.StorageVolumeCallback() {
                        @Override
                        public void onStateChanged(StorageVolume volume) {
                            String desc = volume != null ? volume.getDescription(getContext()) : "";
                            String state = volume != null ? volume.getState() : "";
                            Log.i(TAG, "Storage volume state changed: " + desc + " -> " + state);
                            notifyStorageChanged("volume_state_changed");
                        }
                    };
                    sm.registerStorageVolumeCallback(getContext().getMainExecutor(), cb);
                    volumeCallback = cb;
                }
            } catch (Exception e) {
                Log.w(TAG, "Failed to register storage volume callback: ", e);
            }
        }

        try {
            mediaObserver = new android.database.ContentObserver(new android.os.Handler(android.os.Looper.getMainLooper())) {
                @Override
                public void onChange(boolean selfChange, Uri uri) {
                    super.onChange(selfChange, uri);
                    Log.i(TAG, "MediaStore content changed: " + uri);
                    notifyMediaChanged();
                }
            };
            getContext().getContentResolver().registerContentObserver(
                MediaStore.Images.Media.EXTERNAL_CONTENT_URI, true, mediaObserver
            );
            getContext().getContentResolver().registerContentObserver(
                MediaStore.Video.Media.EXTERNAL_CONTENT_URI, true, mediaObserver
            );
        } catch (Exception e) {
            Log.w(TAG, "Failed to register media content observer: ", e);
        }
    }

    private void notifyStorageChanged(String event) {
        JSObject ret = new JSObject();
        ret.put("event", event);
        ret.put("timestamp", System.currentTimeMillis());
        notifyListeners("storageChanged", ret);
    }

    private void notifyMediaChanged() {
        JSObject ret = new JSObject();
        ret.put("timestamp", System.currentTimeMillis());
        notifyListeners("mediaChanged", ret);
    }

    @Override
    protected void handleOnDestroy() {
        if (mediaReceiver != null) {
            try {
                getContext().unregisterReceiver(mediaReceiver);
            } catch (Exception ignored) {}
            mediaReceiver = null;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && volumeCallback != null) {
            try {
                StorageManager sm = (StorageManager) getContext().getSystemService(Context.STORAGE_SERVICE);
                if (sm != null) {
                    sm.unregisterStorageVolumeCallback((StorageManager.StorageVolumeCallback) volumeCallback);
                }
            } catch (Exception ignored) {}
            volumeCallback = null;
        }
        if (mediaObserver != null) {
            try {
                getContext().getContentResolver().unregisterContentObserver(mediaObserver);
            } catch (Exception ignored) {}
            mediaObserver = null;
        }
        super.handleOnDestroy();
    }

    /**
     * Resolves a virtual path (e.g. "/DCIM/Camera/photo.jpg" or "/Documents/file.pdf")
     * to a physical File on the Android filesystem.
     */
    private File resolveVirtualPath(String virtualPath) {
        if (virtualPath == null || virtualPath.trim().isEmpty() || virtualPath.equals("/")) {
            return Environment.getExternalStorageDirectory();
        }

        // Direct absolute filesystem path (from MediaStore or File.getAbsolutePath())
        if (virtualPath.startsWith("/storage/") || virtualPath.startsWith("/sdcard/") || virtualPath.startsWith("/data/")) {
            return new File(virtualPath);
        }

        // Normalize leading slash
        String path = virtualPath.startsWith("/") ? virtualPath.substring(1) : virtualPath;
        String[] parts = path.split("/", 2);
        String rootKey = parts[0];
        String subPath = parts.length > 1 ? parts[1] : "";

        File baseDir;
        switch (rootKey) {
            case ROOT_DCIM:
                baseDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM);
                break;
            case "Camera":
                File dcimCam = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM), "Camera");
                baseDir = dcimCam.exists() ? dcimCam : Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM);
                break;
            case ROOT_PICTURES:
                baseDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES);
                break;
            case ROOT_DOCUMENTS:
                baseDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS);
                break;
            case ROOT_DOWNLOAD:
                baseDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
                break;
            case ROOT_APP_DATA:
                baseDir = getContext().getExternalFilesDir(null);
                break;
            default:
                // Direct path under external storage
                baseDir = new File(Environment.getExternalStorageDirectory(), rootKey);
                break;
        }

        if (subPath.isEmpty()) {
            return baseDir;
        }
        File res = new File(baseDir, subPath);
        if (!res.exists()) {
            File dcimCamera = new File(new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM), "Camera"), subPath);
            if (dcimCamera.exists()) return dcimCamera;
            File dcimDirect = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM), subPath);
            if (dcimDirect.exists()) return dcimDirect;
            File dcimWithRoot = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM), rootKey + "/" + subPath);
            if (dcimWithRoot.exists()) return dcimWithRoot;
        }
        return res;
    }

    private String getMimeType(File file) {
        return getMimeType(file.getName(), file.isDirectory());
    }

    private String getMimeType(String name, boolean isDir) {
        if (isDir) return "folder";
        int lastDot = name.lastIndexOf('.');
        if (lastDot > 0 && lastDot < name.length() - 1) {
            String ext = name.substring(lastDot + 1).toLowerCase();
            String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
            if (mime != null) return mime;
        }
        return "application/octet-stream";
    }

    private boolean checkStoragePermissions(PluginCall call, String callbackName) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (getPermissionState("media") != PermissionState.GRANTED) {
                requestPermissionForAlias("media", call, callbackName);
                return false;
            }
        } else {
            if (getPermissionState("storage") != PermissionState.GRANTED) {
                requestPermissionForAlias("storage", call, callbackName);
                return false;
            }
        }
        return true;
    }

    /**
     * Returns top-level storage roots available on this Android device.
     */
    @PluginMethod
    public void listRoots(PluginCall call) {
        if (!checkStoragePermissions(call, "listRootsCallback")) {
            return;
        }
        doListRoots(call);
    }

    @PermissionCallback
    private void listRootsCallback(PluginCall call) {
        doListRoots(call);
    }

    private void doListRoots(PluginCall call) {
        try {
            JSArray roots = new JSArray();

            // 1. Camera / DCIM
            File dcim = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DCIM);
            JSObject dcimObj = new JSObject();
            dcimObj.put("id", "/" + ROOT_DCIM);
            dcimObj.put("name", "DCIM");
            dcimObj.put("type", "folder");
            dcimObj.put("isDir", true);
            dcimObj.put("size", 0);
            dcimObj.put("date", dcim != null && dcim.exists() ? dcim.lastModified() : System.currentTimeMillis());
            roots.put(dcimObj);

            // 2. Pictures
            File pic = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES);
            JSObject picObj = new JSObject();
            picObj.put("id", "/" + ROOT_PICTURES);
            picObj.put("name", "Pictures");
            picObj.put("type", "folder");
            picObj.put("isDir", true);
            picObj.put("size", 0);
            picObj.put("date", pic != null && pic.exists() ? pic.lastModified() : System.currentTimeMillis());
            roots.put(picObj);

            // 3. Documents
            File doc = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS);
            JSObject docObj = new JSObject();
            docObj.put("id", "/" + ROOT_DOCUMENTS);
            docObj.put("name", "Documents");
            docObj.put("type", "folder");
            docObj.put("isDir", true);
            docObj.put("size", 0);
            docObj.put("date", doc != null && doc.exists() ? doc.lastModified() : System.currentTimeMillis());
            roots.put(docObj);

            // 4. Downloads
            File dl = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
            JSObject dlObj = new JSObject();
            dlObj.put("id", "/" + ROOT_DOWNLOAD);
            dlObj.put("name", "Downloads");
            dlObj.put("type", "folder");
            dlObj.put("isDir", true);
            dlObj.put("size", 0);
            dlObj.put("date", dl != null && dl.exists() ? dl.lastModified() : System.currentTimeMillis());
            roots.put(dlObj);

            // 5. Removable Storage Volumes (SD Cards, USB OTG, SSDs)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                try {
                    StorageManager sm = (StorageManager) getContext().getSystemService(Context.STORAGE_SERVICE);
                    if (sm != null) {
                        List<StorageVolume> volumes = sm.getStorageVolumes();
                        for (StorageVolume vol : volumes) {
                            if (vol.isRemovable()) {
                                String desc = vol.getDescription(getContext());
                                File volDir = null;
                                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                                    volDir = vol.getDirectory();
                                }
                                String idPath = (volDir != null) ? volDir.getAbsolutePath() : ("/storage/" + vol.getUuid());
                                if (volDir == null || volDir.exists()) {
                                    JSObject volObj = new JSObject();
                                    volObj.put("id", idPath);
                                    volObj.put("name", (desc != null && !desc.isEmpty() ? desc : "SD Card / USB"));
                                    volObj.put("type", "folder");
                                    volObj.put("isDir", true);
                                    volObj.put("size", 0);
                                    volObj.put("date", (volDir != null && volDir.exists()) ? volDir.lastModified() : System.currentTimeMillis());
                                    roots.put(volObj);
                                }
                            }
                        }
                    }
                } catch (Exception e) {
                    Log.w(TAG, "Failed to list removable storage: ", e);
                }
            }

            JSObject ret = new JSObject();
            ret.put("roots", roots);
            call.resolve(ret);
        } catch (Exception e) {
            Log.e(TAG, "listRoots error: ", e);
            call.reject("Failed to list storage roots: " + e.getMessage());
        }
    }

    /**
     * List files and directories within a given virtual path.
     */
    @PluginMethod
    public void list(PluginCall call) {
        if (!checkStoragePermissions(call, "listCallback")) {
            return;
        }
        doList(call);
    }

    @PermissionCallback
    private void listCallback(PluginCall call) {
        doList(call);
    }

    private void doList(PluginCall call) {
        String path = call.getString("path", "/");
        try {
            if (path == null || path.trim().isEmpty() || path.equals("/")) {
                doListRoots(call);
                return;
            }

            File target = resolveVirtualPath(path);
            if (!target.exists()) {
                JSObject ret = new JSObject();
                ret.put("files", new JSArray());
                ret.put("path", path);
                call.resolve(ret);
                return;
            }

            if (!target.isDirectory()) {
                call.reject("Path is not a directory: " + path);
                return;
            }

            File[] files = target.listFiles();
            JSArray filesArr = new JSArray();

            if (files != null) {
                String cleanParent = path.endsWith("/") ? path.substring(0, path.length() - 1) : path;
                for (File f : files) {
                    String name = f.getName();
                    if (name == null || name.startsWith(".")) continue;

                    boolean isDir = f.isDirectory();
                    long size = isDir ? 0 : f.length();
                    long date = f.lastModified();

                    JSObject item = new JSObject();
                    item.put("id", cleanParent + "/" + name);
                    item.put("name", name);
                    item.put("isDir", isDir);
                    item.put("type", getMimeType(name, isDir));
                    item.put("size", size);
                    item.put("date", date);
                    filesArr.put(item);
                }
            }

            JSObject ret = new JSObject();
            ret.put("files", filesArr);
            ret.put("path", path);
            call.resolve(ret);
        } catch (Exception e) {
            Log.e(TAG, "list error for path " + path + ": ", e);
            call.reject("Failed to list path " + path + ": " + e.getMessage());
        }
    }

    /**
     * Stat a file or directory.
     */
    @PluginMethod
    public void stat(PluginCall call) {
        String path = call.getString("path", "");
        try {
            if (path.isEmpty()) {
                call.reject("Path is required");
                return;
            }

            File target = resolveVirtualPath(path);
            JSObject ret = new JSObject();
            if (!target.exists()) {
                ret.put("exists", false);
                call.resolve(ret);
                return;
            }

            ret.put("exists", true);
            ret.put("id", path);
            ret.put("name", target.getName());
            ret.put("isDir", target.isDirectory());
            ret.put("type", getMimeType(target));
            ret.put("size", target.isDirectory() ? 0 : target.length());
            ret.put("date", target.lastModified());
            call.resolve(ret);
        } catch (Exception e) {
            Log.e(TAG, "stat error: ", e);
            call.reject("Failed to stat path: " + e.getMessage());
        }
    }

    /**
     * Reads a slice/chunk of a file for WebRTC DataChannel streaming.
     */
    @PluginMethod
    public void readChunk(PluginCall call) {
        String path = call.getString("path", "");
        long offset = call.getData().optLong("offset", 0L);
        int length = call.getData().optInt("length", 65536);

        if (path.isEmpty()) {
            call.reject("Path is required");
            return;
        }

        try {
            File target = resolveVirtualPath(path);
            if (!target.exists() || !target.isFile()) {
                call.reject("File not found: " + path);
                return;
            }

            long fileLen = target.length();
            if (offset >= fileLen) {
                JSObject ret = new JSObject();
                ret.put("data", "");
                ret.put("bytesRead", 0);
                ret.put("eof", true);
                ret.put("totalSize", fileLen);
                call.resolve(ret);
                return;
            }

            int toRead = (int) Math.min((long) length, fileLen - offset);
            byte[] buffer = new byte[toRead];

            try (RandomAccessFile raf = new RandomAccessFile(target, "r")) {
                raf.seek(offset);
                int read = raf.read(buffer, 0, toRead);
                if (read < 0) read = 0;

                String base64Data = Base64.encodeToString(buffer, 0, read, Base64.NO_WRAP);
                JSObject ret = new JSObject();
                ret.put("data", base64Data);
                ret.put("bytesRead", read);
                ret.put("eof", (offset + read) >= fileLen);
                ret.put("totalSize", fileLen);
                call.resolve(ret);
            }
        } catch (Exception e) {
            Log.e(TAG, "readChunk error: ", e);
            call.reject("Failed to read chunk from " + path + ": " + e.getMessage());
        }
    }

    /**
     * Writes a chunk of data to a file.
     */
    @PluginMethod
    public void writeChunk(PluginCall call) {
        String path = call.getString("path", "");
        long offset = call.getData().optLong("offset", 0L);
        String base64Data = call.getString("data", "");
        boolean append = call.getBoolean("append", false);

        if (path.isEmpty()) {
            call.reject("Path is required");
            return;
        }

        try {
            File target = resolveVirtualPath(path);
            File parent = target.getParentFile();
            if (parent != null && !parent.exists()) {
                parent.mkdirs();
            }

            byte[] bytes = Base64.decode(base64Data, Base64.DEFAULT);

            try (RandomAccessFile raf = new RandomAccessFile(target, "rw")) {
                if (append) {
                    raf.seek(target.length());
                } else {
                    raf.seek(offset);
                }
                raf.write(bytes);
            }

            JSObject ret = new JSObject();
            ret.put("success", true);
            ret.put("bytesWritten", bytes.length);
            ret.put("newSize", target.length());
            call.resolve(ret);
        } catch (Exception e) {
            Log.e(TAG, "writeChunk error: ", e);
            call.reject("Failed to write chunk to " + path + ": " + e.getMessage());
        }
    }

    /**
     * Generates a fast hardware-accelerated thumbnail for images and videos with in-memory caching.
     */
    @PluginMethod
    public void getThumbnail(PluginCall call) {
        String path = call.getString("path", "");
        int width = call.getData().optInt("width", 360);
        int height = call.getData().optInt("height", 360);
        if (path.isEmpty()) {
            call.reject("Path is required");
            return;
        }

        try {
            File target = resolveVirtualPath(path);
            if (!target.exists() || !target.isFile()) {
                call.reject("File not found: " + path);
                return;
            }

            String cacheKey = target.getAbsolutePath() + "_" + target.lastModified() + "_" + width + "x" + height + "_v3";
            JSObject cached = thumbnailCache.get(cacheKey);
            if (cached != null) {
                call.resolve(cached);
                return;
            }

            thumbnailSemaphore.acquire();
            Bitmap bitmap = null;
            try {
                String nameLower = target.getName().toLowerCase();
                String mime = getMimeType(target);
                boolean isVideo = mime.startsWith("video") ||
                        nameLower.endsWith(".mp4") || nameLower.endsWith(".mkv") ||
                        nameLower.endsWith(".mov") || nameLower.endsWith(".avi") ||
                        nameLower.endsWith(".3gp") || nameLower.endsWith(".webm") ||
                        nameLower.endsWith(".ts") || nameLower.endsWith(".m4v");

                if (isVideo) {
                    // Android 10+ (API 29+) hardware-accelerated video thumbnail
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                        try {
                            bitmap = ThumbnailUtils.createVideoThumbnail(target, new Size(width, height), null);
                        } catch (Exception ignored) {}
                    }
                    if (bitmap == null) {
                        MediaMetadataRetriever retriever = new MediaMetadataRetriever();
                        try {
                            retriever.setDataSource(target.getAbsolutePath());
                            // API 27+ hardware scaled extraction directly at target resolution
                            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
                                bitmap = retriever.getScaledFrameAtTime(1000000, MediaMetadataRetriever.OPTION_CLOSEST_SYNC, width, height);
                            }
                            if (bitmap == null) {
                                bitmap = retriever.getFrameAtTime(1000000, MediaMetadataRetriever.OPTION_CLOSEST_SYNC);
                            }
                            if (bitmap == null) {
                                bitmap = retriever.getFrameAtTime(0, MediaMetadataRetriever.OPTION_CLOSEST_SYNC);
                            }
                            if (bitmap == null) {
                                bitmap = retriever.getFrameAtTime();
                            }
                        } catch (Exception ex) {
                            Log.w(TAG, "MediaMetadataRetriever failed for " + path + ": " + ex.getMessage());
                        } finally {
                            try {
                                retriever.release();
                            } catch (Exception ignored) {}
                        }
                    }
                    if (bitmap == null) {
                        try {
                            bitmap = ThumbnailUtils.createVideoThumbnail(target.getAbsolutePath(), MediaStore.Video.Thumbnails.MINI_KIND);
                        } catch (Exception ignored) {}
                    }
                } else {
                    // Decode image with proper sub-sampling and preserve full aspect ratio (no center-cropping)
                    BitmapFactory.Options opts = new BitmapFactory.Options();
                    opts.inJustDecodeBounds = true;
                    BitmapFactory.decodeFile(target.getAbsolutePath(), opts);
                    int maxTargetDim = Math.max(width, height);
                    int sample = 1;
                    while ((opts.outWidth / (sample * 2) >= maxTargetDim) || (opts.outHeight / (sample * 2) >= maxTargetDim)) {
                        sample *= 2;
                    }
                    opts.inJustDecodeBounds = false;
                    opts.inSampleSize = sample;
                    opts.inPreferredConfig = Bitmap.Config.RGB_565;
                    bitmap = BitmapFactory.decodeFile(target.getAbsolutePath(), opts);

                    // Apply EXIF orientation rotation to full uncropped frame
                    if (bitmap != null) {
                        try {
                            android.media.ExifInterface exif = new android.media.ExifInterface(target.getAbsolutePath());
                            int orientation = exif.getAttributeInt(android.media.ExifInterface.TAG_ORIENTATION, android.media.ExifInterface.ORIENTATION_NORMAL);
                            int rotation = 0;
                            boolean flipH = false;
                            boolean flipV = false;

                            switch (orientation) {
                                case android.media.ExifInterface.ORIENTATION_ROTATE_90:
                                    rotation = 90;
                                    break;
                                case android.media.ExifInterface.ORIENTATION_ROTATE_180:
                                    rotation = 180;
                                    break;
                                case android.media.ExifInterface.ORIENTATION_ROTATE_270:
                                    rotation = 270;
                                    break;
                                case android.media.ExifInterface.ORIENTATION_FLIP_HORIZONTAL:
                                    flipH = true;
                                    break;
                                case android.media.ExifInterface.ORIENTATION_FLIP_VERTICAL:
                                    flipV = true;
                                    break;
                                case android.media.ExifInterface.ORIENTATION_TRANSPOSE:
                                    rotation = 90;
                                    flipH = true;
                                    break;
                                case android.media.ExifInterface.ORIENTATION_TRANSVERSE:
                                    rotation = 270;
                                    flipH = true;
                                    break;
                                default:
                                    break;
                            }

                            if (rotation != 0 || flipH || flipV) {
                                android.graphics.Matrix matrix = new android.graphics.Matrix();
                                if (rotation != 0) {
                                    matrix.postRotate(rotation);
                                }
                                if (flipH) {
                                    matrix.postScale(-1, 1);
                                } else if (flipV) {
                                    matrix.postScale(1, -1);
                                }
                                Bitmap rotated = Bitmap.createBitmap(bitmap, 0, 0, bitmap.getWidth(), bitmap.getHeight(), matrix, true);
                                if (rotated != bitmap) {
                                    bitmap.recycle();
                                    bitmap = rotated;
                                }
                            }
                        } catch (Exception ignored) {}
                    }
                }

                if (bitmap != null) {
                    float ratio = Math.min((float) width / bitmap.getWidth(), (float) height / bitmap.getHeight());
                    if (ratio < 0.85f) {
                        int dstW = Math.max(1, Math.round(bitmap.getWidth() * ratio));
                        int dstH = Math.max(1, Math.round(bitmap.getHeight() * ratio));
                        Bitmap scaled = Bitmap.createScaledBitmap(bitmap, dstW, dstH, true);
                        if (scaled != bitmap) {
                            bitmap.recycle();
                            bitmap = scaled;
                        }
                    }
                }
            } finally {
                thumbnailSemaphore.release();
            }

            if (bitmap != null) {
                ByteArrayOutputStream baos = new ByteArrayOutputStream();
                bitmap.compress(Bitmap.CompressFormat.JPEG, 80, baos);
                byte[] jpegBytes = baos.toByteArray();
                bitmap.recycle();
                String base64Data = Base64.encodeToString(jpegBytes, Base64.NO_WRAP);
                JSObject ret = new JSObject();
                ret.put("data", base64Data);
                ret.put("mimeType", "image/jpeg");
                ret.put("size", jpegBytes.length);
                thumbnailCache.put(cacheKey, ret);
                call.resolve(ret);
            } else {
                call.reject("Could not generate thumbnail");
            }
        } catch (Exception e) {
            Log.w(TAG, "getThumbnail error: ", e);
            call.reject("Failed to generate thumbnail: " + e.getMessage());
        }
    }

    @PluginMethod
    public void clearCache(PluginCall call) {
        thumbnailCache.evictAll();
        JSObject ret = new JSObject();
        ret.put("cleared", true);
        call.resolve(ret);
    }

    @PluginMethod
    public void getAppVersion(PluginCall call) {
        try {
            android.content.pm.PackageInfo pInfo = getContext().getPackageManager().getPackageInfo(getContext().getPackageName(), 0);
            JSObject ret = new JSObject();
            ret.put("version", pInfo.versionName);
            long code = 0;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                code = pInfo.getLongVersionCode();
            } else {
                code = pInfo.versionCode;
            }
            ret.put("build", code);
            call.resolve(ret);
        } catch (Exception e) {
            call.reject("Failed to get app version: " + e.getMessage());
        }
    }

    /**
     * Creates a directory.
     */
    @PluginMethod
    public void mkdir(PluginCall call) {
        String path = call.getString("path", "");
        if (path.isEmpty()) {
            call.reject("Path is required");
            return;
        }

        try {
            File target = resolveVirtualPath(path);
            boolean created = target.mkdirs();
            JSObject ret = new JSObject();
            ret.put("success", created || target.exists());
            ret.put("path", path);
            call.resolve(ret);
        } catch (Exception e) {
            Log.e(TAG, "mkdir error: ", e);
            call.reject("Failed to create directory: " + e.getMessage());
        }
    }

    /**
     * Deletes a file or directory.
     */
    @PluginMethod
    public void delete(PluginCall call) {
        String path = call.getString("path", "");
        if (path.isEmpty()) {
            call.reject("Path is required");
            return;
        }

        try {
            File target = resolveVirtualPath(path);
            boolean deleted = deleteRecursive(target);
            JSObject ret = new JSObject();
            ret.put("success", deleted);
            ret.put("path", path);
            call.resolve(ret);
        } catch (Exception e) {
            Log.e(TAG, "delete error: ", e);
            call.reject("Failed to delete " + path + ": " + e.getMessage());
        }
    }

    private boolean deleteRecursive(File fileOrDirectory) {
        if (fileOrDirectory.isDirectory()) {
            File[] children = fileOrDirectory.listFiles();
            if (children != null) {
                for (File child : children) {
                    deleteRecursive(child);
                }
            }
        }
        return fileOrDirectory.delete();
    }

    /**
     * Fast query of Camera & Gallery media via Android MediaStore.
     * Queries images and videos with robust fallbacks and logging.
     */
    @PluginMethod
    public void listMedia(PluginCall call) {
        if (!checkStoragePermissions(call, "listMediaCallback")) {
            return;
        }
        doListMedia(call);
    }

    @PermissionCallback
    private void listMediaCallback(PluginCall call) {
        doListMedia(call);
    }

    private void doListMedia(PluginCall call) {
        int limit = call.getInt("limit", 5000);
        int offset = call.getInt("offset", 0);
        Log.d(TAG, "listMedia called with limit=" + limit + ", offset=" + offset);

        try {
            ContentResolver resolver = getContext().getContentResolver();
            JSArray mediaItems = new JSArray();

            // 1. Query Images
            Uri imageUri = MediaStore.Images.Media.EXTERNAL_CONTENT_URI;
            String[] imgProjection = {
                MediaStore.Images.Media._ID,
                MediaStore.Images.Media.DISPLAY_NAME,
                MediaStore.Images.Media.DATA,
                MediaStore.Images.Media.SIZE,
                MediaStore.Images.Media.DATE_MODIFIED,
                MediaStore.Images.Media.MIME_TYPE
            };

            String sortOrder = MediaStore.MediaColumns.DATE_MODIFIED + " DESC";
            try (Cursor cursor = resolver.query(imageUri, imgProjection, null, null, sortOrder)) {
                if (cursor != null) {
                    int nameCol = cursor.getColumnIndex(MediaStore.Images.Media.DISPLAY_NAME);
                    int dataCol = cursor.getColumnIndex(MediaStore.Images.Media.DATA);
                    int sizeCol = cursor.getColumnIndex(MediaStore.Images.Media.SIZE);
                    int dateCol = cursor.getColumnIndex(MediaStore.Images.Media.DATE_MODIFIED);
                    int mimeCol = cursor.getColumnIndex(MediaStore.Images.Media.MIME_TYPE);

                    while (cursor.moveToNext()) {
                        if (mediaItems.length() >= limit) break;
                        JSObject item = new JSObject();
                        String filePath = dataCol >= 0 ? cursor.getString(dataCol) : null;
                        String name = nameCol >= 0 ? cursor.getString(nameCol) : "";
                        long size = sizeCol >= 0 ? cursor.getLong(sizeCol) : 0;
                        long dateModified = dateCol >= 0 ? cursor.getLong(dateCol) * 1000L : System.currentTimeMillis();
                        String mime = mimeCol >= 0 ? cursor.getString(mimeCol) : "image/jpeg";

                        String pathId = (filePath != null && !filePath.isEmpty()) ? filePath : ("/DCIM/Camera/" + name);
                        item.put("id", pathId);
                        item.put("realPath", pathId);
                        item.put("name", name);
                        item.put("type", mime != null ? mime : "image/jpeg");
                        item.put("size", size);
                        item.put("date", dateModified);
                        item.put("isDir", false);
                        mediaItems.put(item);
                    }
                }
            } catch (Exception e) {
                Log.e(TAG, "Error querying MediaStore Images: ", e);
            }

            // 2. Query Videos
            Uri videoUri = MediaStore.Video.Media.EXTERNAL_CONTENT_URI;
            String[] vidProjection = {
                MediaStore.Video.Media._ID,
                MediaStore.Video.Media.DISPLAY_NAME,
                MediaStore.Video.Media.DATA,
                MediaStore.Video.Media.SIZE,
                MediaStore.Video.Media.DATE_MODIFIED,
                MediaStore.Video.Media.MIME_TYPE
            };
            try (Cursor cursor = resolver.query(videoUri, vidProjection, null, null, sortOrder)) {
                if (cursor != null) {
                    int nameCol = cursor.getColumnIndex(MediaStore.Video.Media.DISPLAY_NAME);
                    int dataCol = cursor.getColumnIndex(MediaStore.Video.Media.DATA);
                    int sizeCol = cursor.getColumnIndex(MediaStore.Video.Media.SIZE);
                    int dateCol = cursor.getColumnIndex(MediaStore.Video.Media.DATE_MODIFIED);
                    int mimeCol = cursor.getColumnIndex(MediaStore.Video.Media.MIME_TYPE);

                    while (cursor.moveToNext()) {
                        if (mediaItems.length() >= limit) break;
                        JSObject item = new JSObject();
                        String filePath = dataCol >= 0 ? cursor.getString(dataCol) : null;
                        String name = nameCol >= 0 ? cursor.getString(nameCol) : "";
                        long size = sizeCol >= 0 ? cursor.getLong(sizeCol) : 0;
                        long dateModified = dateCol >= 0 ? cursor.getLong(dateCol) * 1000L : System.currentTimeMillis();
                        String mime = mimeCol >= 0 ? cursor.getString(mimeCol) : "video/mp4";

                        String pathId = (filePath != null && !filePath.isEmpty()) ? filePath : ("/DCIM/Camera/" + name);
                        item.put("id", pathId);
                        item.put("realPath", pathId);
                        item.put("name", name);
                        item.put("type", mime != null ? mime : "video/mp4");
                        item.put("size", size);
                        item.put("date", dateModified);
                        item.put("isDir", false);
                        mediaItems.put(item);
                    }
                }
            } catch (Exception e) {
                Log.e(TAG, "Error querying MediaStore Videos: ", e);
            }

            Log.d(TAG, "listMedia successfully found " + mediaItems.length() + " items");
            JSObject ret = new JSObject();
            ret.put("items", mediaItems);
            ret.put("count", mediaItems.length());
            call.resolve(ret);
        } catch (Exception e) {
            Log.e(TAG, "listMedia error: ", e);
            call.reject("Failed to list media: " + e.getMessage());
        }
    }
}
