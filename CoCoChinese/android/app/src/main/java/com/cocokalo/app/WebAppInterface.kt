package com.cocokalo.app

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.webkit.JavascriptInterface

class WebAppInterface(private val context: Context) {

    @JavascriptInterface
    fun share(title: String, text: String, url: String) {
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_SUBJECT, title)
            putExtra(Intent.EXTRA_TEXT, "$text\n$url")
        }
        context.startActivity(Intent.createChooser(intent, "分享到"))
    }

    @JavascriptInterface
    fun openUrl(url: String) {
        val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url))
        context.startActivity(intent)
    }

    @JavascriptInterface
    fun getAppVersion(): String {
        return "1.0.0"
    }
}
