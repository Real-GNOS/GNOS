# ProGuard rules for Cocokalo Android App
-keepclassmembers class com.cocokalo.app.WebAppInterface {
    @android.webkit.JavascriptInterface <methods>;
}
-dontwarn android.webkit.**
-keep class android.webkit.** { *; }
