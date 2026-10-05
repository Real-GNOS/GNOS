<template>
  <div class="announcements-page">
    <div class="ann-header">
      <h1><i class="fa fa-bullhorn"></i> 站内公告</h1>
      <p class="ann-sub">平台最新通知与公告</p>
    </div>

    <div v-if="loading" class="ann-loading">
      <i class="fa fa-spinner fa-spin"></i> 加载中…
    </div>

    <div v-else-if="!notices.length" class="ann-empty">
      <i class="fa fa-inbox fa-2x"></i>
      <p>暂无公告</p>
    </div>

    <ul v-else class="ann-list">
      <li v-for="n in notices" :key="n.id" class="ann-item">
        <div class="ann-item-head">
          <h2 class="ann-title">{{ n.title }}</h2>
          <span class="ann-date">{{ formatDate(n.created_at) }}</span>
        </div>
        <p class="ann-content">{{ n.content }}</p>
        <NuxtLink v-if="n.link" :to="n.link" class="ann-link">查看详情 →</NuxtLink>
      </li>
    </ul>
  </div>
</template>

<script setup>
const notices = ref([])
const loading = ref(true)

function formatDate(d) {
  if (!d) return ''
  try { return new Date(d).toLocaleString('zh-CN') } catch { return '' }
}

try {
  const res = await useFetch('/api/notices')
  notices.value = (res.data.value && res.data.value.notices) || []
} catch (e) {
  notices.value = []
} finally {
  loading.value = false
}
</script>

<style scoped>
.announcements-page {
  max-width: 880px;
  margin: 0 auto;
  padding: 32px 20px 60px;
}
.ann-header { margin-bottom: 24px; }
.ann-header h1 { margin: 0 0 6px; font-size: 24px; }
.ann-header h1 i { color: #6c5ce7; margin-right: 8px; }
.ann-sub { margin: 0; color: #888; font-size: 14px; }
.ann-loading, .ann-empty {
  text-align: center;
  color: #999;
  padding: 60px 0;
}
.ann-list { list-style: none; padding: 0; margin: 0; display: flex; flex-direction: column; gap: 16px; }
.ann-item {
  background: #fff;
  border: 1px solid #eee;
  border-radius: 10px;
  padding: 18px 20px;
  box-shadow: 0 2px 8px rgba(0,0,0,0.03);
}
.ann-item-head { display: flex; align-items: baseline; justify-content: space-between; gap: 12px; }
.ann-title { margin: 0; font-size: 17px; font-weight: 600; color: #222; }
.ann-date { font-size: 13px; color: #aaa; white-space: nowrap; }
.ann-content { margin: 10px 0 0; color: #555; line-height: 1.7; white-space: pre-wrap; }
.ann-link { display: inline-block; margin-top: 10px; color: #6c5ce7; text-decoration: none; font-size: 14px; }
.ann-link:hover { text-decoration: underline; }
</style>
