package net.onmydisk.app;

import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.Base64;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;
import android.webkit.ServiceWorkerClient;
import android.webkit.ServiceWorkerController;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;

import androidx.activity.EdgeToEdge;
import androidx.activity.SystemBarStyle;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import com.getcapacitor.BridgeActivity;
import com.getcapacitor.BridgeWebChromeClient;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

public class MainActivity extends BridgeActivity {
    private static final String TAG = "MainActivity";
    private JSONArray pendingShareFiles = null;


    private View customView = null;
    private android.app.Dialog activeWebSheetDialog = null;
    private WebChromeClient.CustomViewCallback customViewCallback = null;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        androidx.core.splashscreen.SplashScreen.installSplashScreen(this);
        setTheme(R.style.AppTheme_NoActionBar);
        supportRequestWindowFeature(android.view.Window.FEATURE_NO_TITLE);
        registerPlugin(NativeStoragePlugin.class);
        registerPlugin(NativeGoogleAuthPlugin.class);
        super.onCreate(savedInstanceState);

        if (getSupportActionBar() != null) {
            getSupportActionBar().hide();
        }
        if (getActionBar() != null) {
            getActionBar().hide();
        }
        enableModernEdgeToEdge();

        // Match system navigation bar and status bar to system dark/light theme
        applySystemBarsTheme();
        setupEdgeToEdgeInsets();
        createPushNotificationChannels();

        // Keep screen on while app is in foreground to prevent sleep and maintain WebRTC / WebSocket connections
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // 1. Enable Service Worker in Android WebView and route localhost requests through Capacitor's local asset server with template parsing
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            try {
                ServiceWorkerController swController = ServiceWorkerController.getInstance();
                swController.setServiceWorkerClient(new ServiceWorkerClient() {
                    @Override
                    public WebResourceResponse shouldInterceptRequest(WebResourceRequest request) {
                        if (request != null && request.getUrl() != null && "localhost".equals(request.getUrl().getHost())) {
                            return interceptAndParseTemplate(request);
                        }
                        return null;
                    }
                });
                Log.d(TAG, "ServiceWorkerClient configured successfully for localhost");
            } catch (Exception e) {
                Log.e(TAG, "Failed to set ServiceWorkerClient", e);
            }
        }

        // 2. Configure WebView settings for media streaming, DOM storage, and P2P workers
        if (getBridge() != null && getBridge().getWebView() != null) {
            WebView webView = getBridge().getWebView();
            WebSettings settings = webView.getSettings();
            settings.setDomStorageEnabled(true);
            settings.setDatabaseEnabled(true);
            settings.setAllowFileAccess(true);
            settings.setAllowContentAccess(true);
            settings.setMediaPlaybackRequiresUserGesture(false);
            webView.addJavascriptInterface(new OmdNativeWebBridge(), "OmdNative");

            // Intercept WebView requests to support OMD template parsing and transparent APK fallback for craft WASM modules
            getBridge().setWebViewClient(new com.getcapacitor.BridgeWebViewClient(getBridge()) {
                @Override
                public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                    if (request != null && request.getUrl() != null && "localhost".equals(request.getUrl().getHost())) {
                        WebResourceResponse resp = interceptAndParseTemplate(request);
                        if (resp != null) {
                            return resp;
                        }
                    }
                    return super.shouldInterceptRequest(view, request);
                }
            });

            webView.setWebChromeClient(new BridgeWebChromeClient(getBridge()) {
                private final android.graphics.Bitmap emptyPoster = android.graphics.Bitmap.createBitmap(1, 1, android.graphics.Bitmap.Config.ARGB_8888);
                @Override
                public android.graphics.Bitmap getDefaultVideoPoster() {
                    return emptyPoster;
                }

                @Override
                public void onShowCustomView(View view, WebChromeClient.CustomViewCallback callback) {
                    if (customView != null) {
                        // Already in fullscreen - dismiss previous request
                        callback.onCustomViewHidden();
                        return;
                    }
                    customView = view;
                    customViewCallback = callback;
                    runOnUiThread(() -> {
                        try {
                            android.view.ViewGroup decor = (android.view.ViewGroup) getWindow().getDecorView();
                            decor.addView(view, new android.widget.FrameLayout.LayoutParams(
                                    android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                                    android.view.ViewGroup.LayoutParams.MATCH_PARENT));

                            WindowInsetsControllerCompat insetsController = WindowCompat.getInsetsController(getWindow(), decor);
                            insetsController.setSystemBarsBehavior(
                                    WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                            insetsController.hide(WindowInsetsCompat.Type.systemBars());
                            View contentView = findViewById(android.R.id.content);
                            if (contentView != null) {
                                ViewCompat.requestApplyInsets(contentView);
                            }
                        } catch (Exception e) {
                            Log.e(TAG, "onShowCustomView failed", e);
                            if (callback != null) callback.onCustomViewHidden();
                        }
                    });
                }

                @Override
                public void onHideCustomView() {
                    if (customView == null) {
                        return;
                    }
                    runOnUiThread(() -> {
                        try {
                            android.view.ViewGroup decor = (android.view.ViewGroup) getWindow().getDecorView();
                            decor.removeView(customView);
                            customView = null;

                            WindowInsetsControllerCompat insetsController = WindowCompat.getInsetsController(getWindow(), decor);
                            insetsController.show(WindowInsetsCompat.Type.systemBars());
                            applySystemBarsTheme();
                            View contentView = findViewById(android.R.id.content);
                            if (contentView != null) {
                                ViewCompat.requestApplyInsets(contentView);
                            }
                        } catch (Exception e) {
                            Log.e(TAG, "onHideCustomView failed", e);
                        } finally {
                            customView = null;
                            customViewCallback = null;
                        }
                    });
                }
            });
        }

        // 3. Handle initial incoming share intent if app was opened via "Отправить На мой диск"
        handleIntent(getIntent());
    }

    @Override
    public void onBackPressed() {
        // Exit WebView fullscreen first if active
        if (exitFullscreen()) {
            return;
        }
        super.onBackPressed();
    }

    private boolean exitFullscreen() {
        if (customView == null) return false;
        runOnUiThread(() -> {
            try {
                android.view.ViewGroup decor = (android.view.ViewGroup) getWindow().getDecorView();
                decor.removeView(customView);
                customView = null;

                WindowInsetsControllerCompat insetsController = WindowCompat.getInsetsController(getWindow(), decor);
                insetsController.show(WindowInsetsCompat.Type.systemBars());
                applySystemBarsTheme();
                View contentView = findViewById(android.R.id.content);
                if (contentView != null) {
                    ViewCompat.requestApplyInsets(contentView);
                }
            } catch (Exception e) {
                Log.e(TAG, "exitFullscreen failed", e);
            }
            if (customViewCallback != null) {
                customViewCallback.onCustomViewHidden();
            }
            customView = null;
            customViewCallback = null;
        });
        return true;
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleIntent(intent);
    }

    private void handleIntent(Intent intent) {
        if (intent == null) return;
        String action = intent.getAction();
        String type = intent.getType();

        if (Intent.ACTION_SEND.equals(action) && type != null) {
            Uri streamUri = intent.getParcelableExtra(Intent.EXTRA_STREAM);
            String extraText = intent.getStringExtra(Intent.EXTRA_TEXT);
            if (streamUri != null) {
                List<Uri> uris = new ArrayList<>();
                uris.add(streamUri);
                processSharedUris(uris);
            } else if (extraText != null) {
                processSharedText(extraText);
            }
        } else if (Intent.ACTION_SEND_MULTIPLE.equals(action) && type != null) {
            ArrayList<Uri> streamUris = intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM);
            if (streamUris != null && !streamUris.isEmpty()) {
                processSharedUris(streamUris);
            }
        }
    }

    private void processSharedUris(List<Uri> uris) {
        new Thread(() -> {
            try {
                JSONArray filesArray = new JSONArray();
                ContentResolver resolver = getContentResolver();

                for (Uri uri : uris) {
                    if (uri == null) continue;
                    JSONObject fileObj = new JSONObject();

                    String name = "shared_file";
                    long size = 0;
                    String mime = resolver.getType(uri);
                    if (mime == null) mime = "application/octet-stream";

                    try (Cursor cursor = resolver.query(uri, null, null, null, null)) {
                        if (cursor != null && cursor.moveToFirst()) {
                            int nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                            int sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE);
                            if (nameIndex != -1) name = cursor.getString(nameIndex);
                            if (sizeIndex != -1) size = cursor.getLong(sizeIndex);
                        }
                    } catch (Exception e) {
                        Log.w(TAG, "Error querying file metadata for uri: " + uri, e);
                    }

                    // Read content into Base64
                    try (InputStream is = resolver.openInputStream(uri);
                         ByteArrayOutputStream buffer = new ByteArrayOutputStream()) {
                        if (is != null) {
                            byte[] data = new byte[16384];
                            int nRead;
                            while ((nRead = is.read(data, 0, data.length)) != -1) {
                                buffer.write(data, 0, nRead);
                            }
                            buffer.flush();
                            byte[] bytes = buffer.toByteArray();
                            if (size == 0) size = bytes.length;
                            String base64Data = Base64.encodeToString(bytes, Base64.NO_WRAP);

                            fileObj.put("name", name);
                            fileObj.put("size", size);
                            fileObj.put("type", mime);
                            fileObj.put("data", base64Data);
                            filesArray.put(fileObj);
                        }
                    } catch (Exception e) {
                        Log.e(TAG, "Error reading content from uri: " + uri, e);
                    }
                }

                if (filesArray.length() > 0) {
                    runOnUiThread(() -> deliverSharedFilesToJS(filesArray));
                }
            } catch (Exception e) {
                Log.e(TAG, "Error processing shared URIs", e);
            }
        }).start();
    }

    private void processSharedText(String text) {
        try {
            JSONArray filesArray = new JSONArray();
            JSONObject fileObj = new JSONObject();
            fileObj.put("name", "shared_text.txt");
            fileObj.put("size", text.getBytes().length);
            fileObj.put("type", "text/plain");
            fileObj.put("data", Base64.encodeToString(text.getBytes(), Base64.NO_WRAP));
            filesArray.put(fileObj);
            runOnUiThread(() -> deliverSharedFilesToJS(filesArray));
        } catch (Exception e) {
            Log.e(TAG, "Error processing shared text", e);
        }
    }

    private void deliverSharedFilesToJS(JSONArray filesArray) {
        pendingShareFiles = filesArray;
        String js = "if (window.__handleAndroidShare) { " +
                "  window.__handleAndroidShare(" + filesArray.toString() + "); " +
                "} else { " +
                "  window.__pendingAndroidShare = " + filesArray.toString() + "; " +
                "}";

        if (getBridge() != null && getBridge().getWebView() != null) {
            getBridge().getWebView().evaluateJavascript(js, null);
        }
    }

    @Override
    public void onResume() {
        super.onResume();
        if (pendingShareFiles != null) {
            deliverSharedFilesToJS(pendingShareFiles);
        }
    }

    private WebResourceResponse interceptAndParseTemplate(WebResourceRequest request) {
        if (request == null || request.getUrl() == null) return null;
        String path = request.getUrl().getPath();
        if (getBridge() == null || getBridge().getLocalServer() == null) return null;

        // Fallback: If heavy static craft editor asset (WordCraft, DeckCraft, GridCraft),
        // check if present in active OTA directory. If omitted from OTA, stream directly from APK assets!
        if (path != null && (path.startsWith("/wordcraft/") || path.startsWith("/deckcraft/") || path.startsWith("/gridcraft/"))) {
            boolean existsInOta = false;
            String basePath = getBridge().getLocalServer().getBasePath();
            if (basePath != null && !basePath.isEmpty()) {
                java.io.File otaFile = new java.io.File(basePath, path.startsWith("/") ? path.substring(1) : path);
                existsInOta = otaFile.exists() && otaFile.isFile();
            }
            if (!existsInOta) {
                try {
                    String assetPath = "public" + (path.startsWith("/") ? path : "/" + path);
                    InputStream is = getAssets().open(assetPath);
                    String mime = getMimeTypeForPath(path);
                    return new WebResourceResponse(mime, "UTF-8", is);
                } catch (Exception e) {
                    Log.w(TAG, "Fallback craft asset not found in APK for: " + path, e);
                }
            }
        }

        WebResourceResponse resp = getBridge().getLocalServer().shouldInterceptRequest(request);

        if (resp != null && (path == null || path.isEmpty() || path.equals("/") || path.endsWith(".html"))) {
            try {
                InputStream is = resp.getData();
                if (is != null) {
                    ByteArrayOutputStream buffer = new ByteArrayOutputStream();
                    byte[] data = new byte[8192];
                    int nRead;
                    while ((nRead = is.read(data, 0, data.length)) != -1) {
                        buffer.write(data, 0, nRead);
                    }
                    String originalHtml = buffer.toString("UTF-8");
                    String parsedHtml = parseOmdTemplate(originalHtml);
                    byte[] parsedBytes = parsedHtml.getBytes(java.nio.charset.StandardCharsets.UTF_8);
                    return new WebResourceResponse(
                        "text/html",
                        "UTF-8",
                        resp.getStatusCode(),
                        resp.getReasonPhrase(),
                        resp.getResponseHeaders(),
                        new java.io.ByteArrayInputStream(parsedBytes)
                    );
                }
            } catch (Exception e) {
                Log.w(TAG, "Error parsing template for path: " + path, e);
            }
        }
        return resp;
    }

    private static String getMimeTypeForPath(String path) {
        if (path.endsWith(".wasm")) return "application/wasm";
        if (path.endsWith(".js") || path.endsWith(".mjs")) return "application/javascript";
        if (path.endsWith(".html")) return "text/html";
        if (path.endsWith(".css")) return "text/css";
        if (path.endsWith(".png")) return "image/png";
        if (path.endsWith(".ico")) return "image/x-icon";
        if (path.endsWith(".svg")) return "image/svg+xml";
        if (path.endsWith(".json")) return "application/json";
        return "application/octet-stream";
    }

    public static String parseOmdTemplate(String html) {
        if (html == null) return "";
        // 1. Strip {if ...}...{end ...} conditional blocks
        html = html.replaceAll("(?s)\\{if\\s+[a-zA-Z0-9_]+\\}[^{]*?\\{end\\s+[a-zA-Z0-9_]+\\}", "");

        // 2. Replace known template variables
        html = html.replace("{TITLE}", "On My Disk")
                   .replace("{APPTITLE}", "On My Disk")
                   .replace("{DESCRIPTION}", "Secure P2P file sharing")
                   .replace("{OWNER}", "OnMyDisk")
                   .replace("{CARD}", "summary_large_image")
                   .replace("{SERVERURL}", "")
                   .replace("{LINKURL}", "")
                   .replace("{COVERVERSION}", "0")
                   .replace("{IMG_WIDTH}", "1200")
                   .replace("{IMG_HEIGHT}", "630");
        return html;
    }

    @Override
    public void onConfigurationChanged(android.content.res.Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        applySystemBarsTheme();
    }

    private void enableModernEdgeToEdge() {
        int nightModeFlags = getResources().getConfiguration().uiMode & android.content.res.Configuration.UI_MODE_NIGHT_MASK;
        boolean isNight = nightModeFlags == android.content.res.Configuration.UI_MODE_NIGHT_YES;
        int darkColor = android.graphics.Color.parseColor("#101C22");
        int lightColor = android.graphics.Color.parseColor("#F6F8F9");

        SystemBarStyle statusBarStyle = isNight
            ? SystemBarStyle.dark(darkColor)
            : SystemBarStyle.light(lightColor, darkColor);
        SystemBarStyle navBarStyle = isNight
            ? SystemBarStyle.dark(darkColor)
            : SystemBarStyle.light(lightColor, darkColor);

        EdgeToEdge.enable(this, statusBarStyle, navBarStyle);
    }

    private void setupEdgeToEdgeInsets() {
        View contentView = findViewById(android.R.id.content);
        if (contentView != null) {
            ViewCompat.setOnApplyWindowInsetsListener(contentView, (v, windowInsets) -> {
                if (customView != null) {
                    v.setPadding(0, 0, 0, 0);
                    return windowInsets;
                }
                Insets insets = windowInsets.getInsets(
                    WindowInsetsCompat.Type.systemBars()
                    | WindowInsetsCompat.Type.displayCutout()
                    | WindowInsetsCompat.Type.ime()
                );
                v.setPadding(insets.left, insets.top, insets.right, insets.bottom);
                return WindowInsetsCompat.CONSUMED;
            });
            ViewCompat.requestApplyInsets(contentView);
        }
    }

    private void applySystemBarsTheme() {
        int nightModeFlags = getResources().getConfiguration().uiMode & android.content.res.Configuration.UI_MODE_NIGHT_MASK;
        boolean isNight = nightModeFlags == android.content.res.Configuration.UI_MODE_NIGHT_YES;
        int barBgColor = android.graphics.Color.parseColor(isNight ? "#101C22" : "#F6F8F9");

        View decorView = getWindow().getDecorView();
        decorView.setBackgroundColor(barBgColor);
        View contentView = findViewById(android.R.id.content);
        if (contentView != null) {
            contentView.setBackgroundColor(barBgColor);
        }

        WindowInsetsControllerCompat insetsController = WindowCompat.getInsetsController(getWindow(), decorView);
        insetsController.setAppearanceLightStatusBars(!isNight);
        insetsController.setAppearanceLightNavigationBars(!isNight);
    }

    private void createPushNotificationChannels() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            try {
                android.app.NotificationManager nm = getSystemService(android.app.NotificationManager.class);
                if (nm != null) {
                    android.app.NotificationChannel ch = new android.app.NotificationChannel(
                        "omd_autopilot_high",
                        "On My Disk AI & Moderation",
                        android.app.NotificationManager.IMPORTANCE_HIGH
                    );
                    ch.setDescription("Rich notifications and moderation alerts from your On My Disk AI node");
                    ch.enableVibration(true);
                    nm.createNotificationChannel(ch);
                }
            } catch (Exception e) {
                Log.w(TAG, "Failed to create notification channel", e);
            }
        }
    }

    public class OmdNativeWebBridge {
        @android.webkit.JavascriptInterface
        public void copyToClipboard(String text) {
            if (text == null) return;
            runOnUiThread(() -> {
                try {
                    android.content.ClipboardManager cb = (android.content.ClipboardManager) getSystemService(android.content.Context.CLIPBOARD_SERVICE);
                    if (cb != null) {
                        cb.setPrimaryClip(android.content.ClipData.newPlainText("text", text));
                    }
                } catch (Exception e) {
                    Log.w(TAG, "Clipboard copy failed", e);
                }
            });
        }

        @android.webkit.JavascriptInterface
        public void openWebSheet(String url, String title) {
            if (url == null || (!url.startsWith("https://") && !url.startsWith("http://"))) {
                return;
            }
            runOnUiThread(() -> showInAppWebSheet(url, title));
        }
    }

    private int dpToPx(int dp) {
        return Math.round(dp * getResources().getDisplayMetrics().density);
    }

    private void showInAppWebSheet(final String initialUrl, final String sheetTitle) {
        try {
            if (activeWebSheetDialog != null && activeWebSheetDialog.isShowing()) {
                try {
                    activeWebSheetDialog.dismiss();
                } catch (Exception ignored) {}
            }

            final android.app.Dialog dialog = new android.app.Dialog(this, android.R.style.Theme_DeviceDefault_NoActionBar);
            activeWebSheetDialog = dialog;

            android.widget.LinearLayout root = new android.widget.LinearLayout(this);
            root.setOrientation(android.widget.LinearLayout.VERTICAL);
            android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
            bg.setColor(android.graphics.Color.parseColor("#131C24"));
            float r = dpToPx(18);
            bg.setCornerRadii(new float[]{r, r, r, r, 0f, 0f, 0f, 0f});
            root.setBackground(bg);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                root.setClipToOutline(true);
            }

            // Header bar
            android.widget.LinearLayout header = new android.widget.LinearLayout(this);
            header.setOrientation(android.widget.LinearLayout.HORIZONTAL);
            header.setGravity(android.view.Gravity.CENTER_VERTICAL);
            header.setBackgroundColor(android.graphics.Color.parseColor("#182430"));
            header.setPadding(dpToPx(14), dpToPx(10), dpToPx(10), dpToPx(10));

            android.widget.LinearLayout titleBox = new android.widget.LinearLayout(this);
            titleBox.setOrientation(android.widget.LinearLayout.VERTICAL);
            android.widget.LinearLayout.LayoutParams titleLp = new android.widget.LinearLayout.LayoutParams(
                0, android.view.ViewGroup.LayoutParams.WRAP_CONTENT, 1f
            );
            titleBox.setLayoutParams(titleLp);

            final android.widget.TextView titleTv = new android.widget.TextView(this);
            titleTv.setText((sheetTitle != null && !sheetTitle.isEmpty()) ? sheetTitle : "Web");
            titleTv.setTextColor(android.graphics.Color.parseColor("#EEF2F7"));
            titleTv.setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 15);
            titleTv.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            titleTv.setSingleLine(true);
            titleTv.setEllipsize(android.text.TextUtils.TruncateAt.END);

            final android.widget.TextView subTv = new android.widget.TextView(this);
            Uri parsedInit = Uri.parse(initialUrl);
            subTv.setText(parsedInit.getHost() != null ? parsedInit.getHost() : initialUrl);
            subTv.setTextColor(android.graphics.Color.parseColor("#7DD3FC"));
            subTv.setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 11.5f);
            subTv.setSingleLine(true);
            subTv.setEllipsize(android.text.TextUtils.TruncateAt.END);

            titleBox.addView(titleTv);
            titleBox.addView(subTv);

            final WebView sheetWebView = new WebView(this);

            android.widget.TextView reloadBtn = new android.widget.TextView(this);
            reloadBtn.setText("↻");
            reloadBtn.setTextColor(android.graphics.Color.parseColor("#CBD5E1"));
            reloadBtn.setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 18);
            reloadBtn.setPadding(dpToPx(10), dpToPx(4), dpToPx(10), dpToPx(4));
            reloadBtn.setOnClickListener(v -> sheetWebView.reload());

            android.widget.TextView closeBtn = new android.widget.TextView(this);
            closeBtn.setText("✕");
            closeBtn.setTextColor(android.graphics.Color.parseColor("#94A3B8"));
            closeBtn.setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 18);
            closeBtn.setPadding(dpToPx(12), dpToPx(4), dpToPx(8), dpToPx(4));
            closeBtn.setOnClickListener(v -> dialog.dismiss());

            header.addView(titleBox);
            header.addView(reloadBtn);
            header.addView(closeBtn);

            final android.widget.ProgressBar progressBar = new android.widget.ProgressBar(
                this, null, android.R.attr.progressBarStyleHorizontal
            );
            progressBar.setMax(100);
            progressBar.setLayoutParams(new android.widget.LinearLayout.LayoutParams(
                android.view.ViewGroup.LayoutParams.MATCH_PARENT, dpToPx(2)
            ));

            sheetWebView.setLayoutParams(new android.widget.LinearLayout.LayoutParams(
                android.view.ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f
            ));
            sheetWebView.setBackgroundColor(android.graphics.Color.parseColor("#0F171E"));
            sheetWebView.setFocusable(true);
            sheetWebView.setFocusableInTouchMode(true);

            WebSettings ws = sheetWebView.getSettings();
            ws.setJavaScriptEnabled(true);
            ws.setDomStorageEnabled(true);
            ws.setDatabaseEnabled(true);
            ws.setJavaScriptCanOpenWindowsAutomatically(true);
            ws.setSupportMultipleWindows(true);
            ws.setLoadWithOverviewMode(true);
            ws.setUseWideViewPort(true);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                ws.setMixedContentMode(WebSettings.MIXED_CONTENT_COMPATIBILITY_MODE);
            }
            String ua = ws.getUserAgentString();
            if (ua != null) {
                ua = ua.replace("; wv)", ")").replace("Version/4.0 ", "");
                ws.setUserAgentString(ua);
            }

            android.webkit.CookieManager cm = android.webkit.CookieManager.getInstance();
            cm.setAcceptCookie(true);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                cm.setAcceptThirdPartyCookies(sheetWebView, true);
            }

            sheetWebView.setWebViewClient(new android.webkit.WebViewClient() {
                @Override
                public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                    Uri u = request != null ? request.getUrl() : null;
                    if (u == null) return false;
                    String scheme = u.getScheme() != null ? u.getScheme().toLowerCase() : "";
                    if ("http".equals(scheme) || "https".equals(scheme)) {
                        return false; // Keep strictly inside in-app sheet WebView
                    }
                    // Block custom app-bounce schemes (twitter://, intent://, x://) so native app cannot hijack and strip in_reply_to
                    return true;
                }

                @SuppressWarnings("deprecation")
                @Override
                public boolean shouldOverrideUrlLoading(WebView view, String urlStr) {
                    if (urlStr == null) return false;
                    String lower = urlStr.toLowerCase();
                    if (lower.startsWith("https://") || lower.startsWith("http://")) {
                        return false;
                    }
                    return true;
                }

                @Override
                public void onPageStarted(WebView view, String pageUrl, android.graphics.Bitmap favicon) {
                    super.onPageStarted(view, pageUrl, favicon);
                    if (pageUrl != null) {
                        Uri u = Uri.parse(pageUrl);
                        if (u.getHost() != null) {
                            subTv.setText(u.getHost());
                        }
                    }
                }

                @Override
                public void onPageFinished(WebView view, String pageUrl) {
                    super.onPageFinished(view, pageUrl);
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                        android.webkit.CookieManager.getInstance().flush();
                    }
                }
            });

            sheetWebView.setWebChromeClient(new WebChromeClient() {
                @Override
                public void onProgressChanged(WebView view, int newProgress) {
                    progressBar.setProgress(newProgress);
                    progressBar.setVisibility(newProgress >= 100 ? View.GONE : View.VISIBLE);
                }

                @Override
                public void onCloseWindow(WebView window) {
                    try {
                        dialog.dismiss();
                    } catch (Exception ignored) {}
                }

                @Override
                public boolean onCreateWindow(WebView view, boolean isDialog, boolean isUserGesture, android.os.Message resultMsg) {
                    WebView tempWv = new WebView(MainActivity.this);
                    tempWv.setWebViewClient(new android.webkit.WebViewClient() {
                        @Override
                        public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest req) {
                            Uri u = req != null ? req.getUrl() : null;
                            if (u != null && ("http".equalsIgnoreCase(u.getScheme()) || "https".equalsIgnoreCase(u.getScheme()))) {
                                sheetWebView.loadUrl(u.toString());
                            }
                            v.destroy();
                            return true;
                        }
                    });
                    WebView.WebViewTransport transport = (WebView.WebViewTransport) resultMsg.obj;
                    transport.setWebView(tempWv);
                    resultMsg.sendToTarget();
                    return true;
                }
            });

            root.addView(header);
            root.addView(progressBar);
            root.addView(sheetWebView);

            dialog.setContentView(root);
            android.view.Window win = dialog.getWindow();
            if (win != null) {
                win.setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(android.graphics.Color.TRANSPARENT));
                int screenH = getResources().getDisplayMetrics().heightPixels;
                win.setLayout(
                    WindowManager.LayoutParams.MATCH_PARENT,
                    Math.round(screenH * 0.90f)
                );
                win.setGravity(android.view.Gravity.BOTTOM);
                win.clearFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM);
                ViewCompat.setOnApplyWindowInsetsListener(root, (v, windowInsets) -> {
                    Insets imeOrNav = windowInsets.getInsets(
                        WindowInsetsCompat.Type.ime() | WindowInsetsCompat.Type.navigationBars()
                    );
                    v.setPadding(0, 0, 0, imeOrNav.bottom);
                    return WindowInsetsCompat.CONSUMED;
                });
            }

            dialog.setOnKeyListener((d, keyCode, event) -> {
                if (keyCode == android.view.KeyEvent.KEYCODE_BACK && event.getAction() == android.view.KeyEvent.ACTION_UP) {
                    if (sheetWebView.canGoBack()) {
                        sheetWebView.goBack();
                        return true;
                    }
                    d.dismiss();
                    return true;
                }
                return false;
            });

            dialog.setOnDismissListener(d -> {
                try {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                        android.webkit.CookieManager.getInstance().flush();
                    }
                    root.removeView(sheetWebView);
                    sheetWebView.stopLoading();
                    sheetWebView.destroy();
                } catch (Exception ignored) {}
                if (activeWebSheetDialog == d) {
                    activeWebSheetDialog = null;
                }
            });

            dialog.show();
            sheetWebView.requestFocus();
            sheetWebView.loadUrl(initialUrl);
        } catch (Exception e) {
            Log.e(TAG, "Failed to open in-app web sheet", e);
        }
    }
}
