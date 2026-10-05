package com.cocokalo.app

import android.annotation.SuppressLint
import android.content.Intent
import android.graphics.Bitmap
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.view.View
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.ProgressBar
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout

class MainActivity : AppCompatActivity() {

    private lateinit var webView: WebView
    private lateinit var swipeRefresh: SwipeRefreshLayout
    private lateinit var progressBar: ProgressBar
    private lateinit var splashContainer: FrameLayout
    private lateinit var errorContainer: View
    private lateinit var mainContent: View

    private var splashDone = false
    private var pageLoaded = false
    private val homeUrl: String by lazy { getString(R.string.base_url) }

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        setContentView(R.layout.activity_main)

        webView = findViewById(R.id.webView)
        swipeRefresh = findViewById(R.id.swipeRefresh)
        progressBar = findViewById(R.id.progressBar)
        splashContainer = findViewById(R.id.splashContainer)
        errorContainer = findViewById(R.id.errorContainer)
        mainContent = findViewById(R.id.mainContent)

        setupWebView()
        setupSwipeRefresh()
        setupErrorView()

        webView.loadUrl(homeUrl)

        splashContainer.postDelayed({
            splashDone = true
            checkDismissSplash()
        }, 1500)
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun setupWebView() {
        webView.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            allowFileAccess = false
            useWideViewPort = true
            loadWithOverviewMode = true
            builtInZoomControls = true
            displayZoomControls = false
            setSupportZoom(true)
            mediaPlaybackRequiresUserGesture = false
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                safeBrowsingEnabled = true
            }
        }

        webView.webChromeClient = object : WebChromeClient() {
            override fun onReceivedTitle(view: WebView?, title: String?) {
                supportActionBar?.title = title
            }
        }

        webView.webViewClient = object : WebViewClient() {
            override fun onPageStarted(view: WebView?, url: String?, favicon: Bitmap?) {
                progressBar.visibility = View.VISIBLE
                errorContainer.visibility = View.GONE
            }

            override fun onPageFinished(view: WebView?, url: String?) {
                progressBar.visibility = View.GONE
                swipeRefresh.isRefreshing = false
                pageLoaded = true
                checkDismissSplash()
            }

            override fun onReceivedError(
                view: WebView?,
                request: WebResourceRequest?,
                error: android.webkit.WebResourceError?
            ) {
                if (request?.isForMainFrame == true) {
                    errorContainer.visibility = View.VISIBLE
                    webView.visibility = View.GONE
                }
            }

            override fun shouldOverrideUrlLoading(
                view: WebView?,
                request: WebResourceRequest?
            ): Boolean {
                val url = request?.url?.toString() ?: return false
                return if (url.startsWith("tel:") || url.startsWith("mailto:")) {
                    startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
                    true
                } else {
                    false
                }
            }
        }

        webView.addJavascriptInterface(WebAppInterface(this), "Android")
    }

    private fun setupSwipeRefresh() {
        swipeRefresh.setColorSchemeColors(
            getColor(R.color.cocokalo_blue),
            getColor(android.R.color.holo_orange_light)
        )
        swipeRefresh.setOnRefreshListener {
            errorContainer.visibility = View.GONE
            webView.visibility = View.VISIBLE
            webView.reload()
        }
    }

    private fun setupErrorView() {
        errorContainer.findViewById<TextView>(R.id.retryBtn)?.setOnClickListener {
            errorContainer.visibility = View.GONE
            webView.visibility = View.VISIBLE
            webView.reload()
        }
    }

    private fun checkDismissSplash() {
        if (splashDone && pageLoaded) {
            splashContainer.animate()
                .alpha(0f)
                .setDuration(300)
                .withEndAction {
                    splashContainer.visibility = View.GONE
                    mainContent.visibility = View.VISIBLE
                    mainContent.alpha = 0f
                    mainContent.animate().alpha(1f).setDuration(200).start()
                }
                .start()
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (webView.canGoBack()) {
            webView.goBack()
        } else {
            super.onBackPressed()
        }
    }

    override fun onResume() {
        super.onResume()
        webView.onResume()
    }

    override fun onPause() {
        super.onPause()
        webView.onPause()
    }

    override fun onDestroy() {
        webView.destroy()
        super.onDestroy()
    }
}
