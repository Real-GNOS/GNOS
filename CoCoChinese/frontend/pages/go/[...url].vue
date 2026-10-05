<template>
  <div class="go-page">
    <div class="go-card">
      <div class="go-icon">
        <i class="fa fa-external-link-square"></i>
      </div>
      <h2>外部链接跳转</h2>
      <p class="go-warn">你即将离开本页面，跳转到以下外部链接：</p>
      <div class="go-url-box">
        <i class="fa fa-link me-2"></i>
        <a :href="targetUrl" target="_blank" rel="noopener noreferrer" class="go-url-text">{{ targetUrl }}</a>
      </div>
      <div class="go-tips">
        <p><i class="fa fa-shield me-1"></i> 安全提示</p>
        <ul>
          <li>请注意辨别链接内容的真实性</li>
          <li>不要在可疑网站输入个人敏感信息</li>
          <li>本站不对第三方内容的安全性负责</li>
        </ul>
      </div>
      <div class="go-actions">
        <button class="btn btn-secondary" @click="goBack">
          <i class="fa fa-arrow-left me-1"></i>返回上一页
        </button>
        <a :href="targetUrl" target="_blank" rel="noopener noreferrer" class="btn btn-primary" @click="confirmed = true">
          继续前往<i class="fa fa-arrow-right ms-1"></i>
        </a>
      </div>
    </div>
  </div>
</template>

<script setup>
const route = useRoute()
const confirmed = ref(false)

const targetUrl = computed(() => {
  const raw = route.params.url
  if (Array.isArray(raw)) return raw.join('/')
  return raw || ''
})

function goBack() {
  if (window.history.length > 1) {
    window.history.back()
  } else {
    navigateTo('/')
  }
}

useHead({
  title: '外部链接跳转',
})
</script>

<style scoped>
.go-page {
  min-height: 80vh;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 2rem;
}
.go-card {
  max-width: 520px;
  width: 100%;
  background: var(--bg-card, #fff);
  border-radius: 12px;
  box-shadow: 0 2px 16px rgba(0,0,0,0.08);
  padding: 2.5rem;
  text-align: center;
}
.go-icon {
  font-size: 2.5rem;
  color: #f0ad4e;
  margin-bottom: 1rem;
}
.go-card h2 {
  margin-bottom: 0.5rem;
  font-size: 1.4rem;
}
.go-warn {
  color: #666;
  margin-bottom: 1.2rem;
  font-size: 0.95rem;
}
.go-url-box {
  background: #f5f5f5;
  border: 1px solid #e0e0e0;
  border-radius: 8px;
  padding: 0.8rem 1rem;
  margin-bottom: 1.2rem;
  word-break: break-all;
  font-size: 0.9rem;
}
.go-url-text {
  color: #1a73e8;
  text-decoration: none;
}
.go-url-text:hover {
  text-decoration: underline;
}
.go-tips {
  text-align: left;
  background: #fffbe6;
  border: 1px solid #ffe58f;
  border-radius: 8px;
  padding: 1rem;
  margin-bottom: 1.5rem;
  font-size: 0.85rem;
  color: #614700;
}
.go-tips p {
  margin-bottom: 0.5rem;
  font-weight: 600;
}
.go-tips ul {
  margin: 0;
  padding-left: 1.2rem;
}
.go-tips li {
  margin-bottom: 0.3rem;
}
.go-actions {
  display: flex;
  gap: 1rem;
  justify-content: center;
}
.go-actions .btn {
  min-width: 140px;
}
</style>
