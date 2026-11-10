# Preserve line numbers and annotations for crash diagnostics & Capacitor reflection
-keepattributes SourceFile,LineNumberTable,*Annotation*,JavascriptInterface,Signature,InnerClasses,EnclosingMethod
-renamesourcefileattribute SourceFile

# Keep Capacitor core bridge & plugin reflection
-keep class com.getcapacitor.** { *; }
-keep @com.getcapacitor.annotation.CapacitorPlugin class * { *; }
-keepclassmembers class * extends com.getcapacitor.Plugin {
    @com.getcapacitor.PluginMethod public <methods>;
}

# Keep On My Disk native classes & registered Capacitor plugins
-keep class net.onmydisk.app.** { *; }
-keep class com.capacitorjs.plugins.pushnotifications.** { *; }
-keep class ee.forgr.capacitor_updater.** { *; }
-keep class com.getcapacitor.community.applesignin.** { *; }

# Keep AndroidX Credential Manager & Google Identity reflection providers
-keep class androidx.credentials.** { *; }
-keep class com.google.android.libraries.identity.googleid.** { *; }
-keep class com.google.android.gms.auth.** { *; }
-keep class com.google.android.gms.common.** { *; }

# Keep WebView @JavascriptInterface bridges
-keepclassmembers class * {
    @android.webkit.JavascriptInterface <methods>;
}

# Suppress benign optional library warnings during R8 optimization
-dontwarn org.slf4j.**
-dontwarn javax.annotation.**
