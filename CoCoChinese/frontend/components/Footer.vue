<template>
  <footer class="site-footer">
    <div class="footer-inner">
      <div class="footer-main">
        <div class="footer-section footer-brand">
          <h3 class="footer-logo">Cocokalo</h3>
          <p class="footer-desc">分享你的热爱，发现精彩世界</p>
          <div class="footer-social">
            <a v-for="item in footer.imgSend" :key="item.text" :href="item.link" class="social-link" :title="item.text">
              <i :class="'fa ' + item.fontClass"></i>
            </a>
          </div>
        </div>
        <div class="footer-section">
          <h4 class="footer-title">关于我们</h4>
          <ul class="footer-links">
            <li v-for="item in footer.about" :key="item.text">
              <NuxtLink :to="item.link">{{ item.text }}</NuxtLink>
            </li>
          </ul>
        </div>
        <div class="footer-section">
          <h4 class="footer-title">传送门</h4>
          <ul class="footer-links">
            <li v-for="item in footer.send" :key="item.text">
              <NuxtLink :to="item.link">{{ item.text }}</NuxtLink>
            </li>
          </ul>
        </div>
        <div class="footer-section footer-download">
          <h4 class="footer-title">客户端下载</h4>
          <div class="download-badges">
            <a href="/" class="download-btn">
              <i class="fa fa-android"></i> Android
            </a>
          </div>
        </div>
      </div>
      <div class="footer-bottom">
        <p>Copyright &copy; Cocokalo. All Rights Reserved.</p>
      </div>
    </div>
  </footer>
</template>

<script setup>
defineProps({ footer: { type: Object, default: () => ({ about: [], send: [], imgSend: [] }) } })
</script>

<style scoped>
.site-footer {
  background: #f6f9fc;
  border-top: 1px solid #e8f0f6;
  margin-top: 60px;
  padding: 40px 0 0;
}
.footer-inner {
  max-width: 1300px;
  margin: 0 auto;
  padding: 0 24px;
}
.footer-main {
  display: grid;
  grid-template-columns: 1.5fr 1fr 1fr 1fr;
  gap: 40px;
  padding-bottom: 32px;
}
.footer-brand { padding-right: 20px; }
.footer-logo {
  font-size: 22px;
  font-weight: 700;
  color: #00a1d6;
  margin: 0 0 8px;
}
.footer-desc {
  font-size: 13px;
  color: #99a2aa;
  margin: 0 0 16px;
  line-height: 1.6;
}
.footer-social {
  display: flex;
  gap: 12px;
}
.social-link {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 36px;
  height: 36px;
  border-radius: 50%;
  background: #e8f0f6;
  color: #666;
  text-decoration: none;
  font-size: 16px;
  transition: all 0.2s;
}
.social-link:hover {
  background: #00a1d6;
  color: #fff;
}
.footer-title {
  font-size: 14px;
  font-weight: 600;
  color: #333;
  margin: 0 0 14px;
}
.footer-links {
  list-style: none;
  padding: 0;
  margin: 0;
}
.footer-links li {
  margin-bottom: 8px;
}
.footer-links a {
  font-size: 13px;
  color: #666;
  text-decoration: none;
  transition: color 0.2s;
}
.footer-links a:hover {
  color: #00a1d6;
}
.download-badges {
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.download-btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 8px 16px;
  border: 1px solid #d0d7de;
  border-radius: 6px;
  color: #555;
  font-size: 13px;
  text-decoration: none;
  transition: all 0.2s;
}
.download-btn:hover {
  border-color: #00a1d6;
  color: #00a1d6;
  background: rgba(0, 161, 214, 0.04);
}
.footer-bottom {
  border-top: 1px solid #e8f0f6;
  padding: 16px 0;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 12px;
  font-size: 12px;
  color: #99a2aa;
  flex-wrap: wrap;
}
.footer-bottom p { margin: 0; }
.icp-link {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  color: #99a2aa;
  text-decoration: none;
}
.icp-link:hover { color: #666; }
@media (max-width: 768px) {
  .footer-main {
    grid-template-columns: 1fr 1fr;
    gap: 24px;
  }
  .footer-brand { grid-column: 1 / -1; }
}
@media (max-width: 480px) {
  .footer-main { grid-template-columns: 1fr; }
}
</style>
