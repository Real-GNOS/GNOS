<template>
  <div class="forum-page">
    <div class="forum-container">
      <div class="forum-header">
        <h1>论坛</h1>
        <p class="forum-subtitle">欢迎来到 Cocokalo 社区，畅所欲言</p>
      </div>

      <div class="category-grid">
        <div v-for="cat in categories" :key="cat.id" class="category-card" @click="navigateTo(`/forum/category/${cat.slug}`)">
          <div class="category-icon">
            <i :class="'fa ' + (cat.icon || 'fa-comments')"></i>
          </div>
          <div class="category-info">
            <h3 class="category-name">{{ cat.name }}</h3>
            <p class="category-desc">{{ cat.description }}</p>
          </div>
          <div class="category-stats">
            <div class="stat-item">
              <span class="stat-num">{{ cat.topic_count }}</span>
              <span class="stat-label">话题</span>
            </div>
            <div class="stat-item">
              <span class="stat-num">{{ cat.post_count }}</span>
              <span class="stat-label">帖子</span>
            </div>
          </div>
          <div class="category-arrow">
            <i class="fa fa-chevron-right"></i>
          </div>
        </div>
      </div>

      <div class="forum-info-bar">
        <div class="info-item">
          <i class="fa fa-file-text"></i>
          <span>总话题: <strong>{{ stats.totalTopics }}</strong></span>
        </div>
        <div class="info-item">
          <i class="fa fa-reply"></i>
          <span>总帖子: <strong>{{ stats.totalPosts }}</strong></span>
        </div>
        <div class="info-item">
          <i class="fa fa-users"></i>
          <span>在线: <strong>{{ stats.onlineUsers }}</strong></span>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
const { data: catData } = await useFetch('/api/forum/categories')
const categories = ref(catData.value?.data || [])

const stats = computed(() => {
  const totalTopics = categories.value.reduce((s, c) => s + (parseInt(c.topic_count) || 0), 0)
  const totalPosts = categories.value.reduce((s, c) => s + (parseInt(c.post_count) || 0), 0)
  return { totalTopics, totalPosts, onlineUsers: '-' }
})
</script>

<style scoped>
.forum-page {
  min-height: calc(100vh - 60px);
  background: #f4f4f5;
  padding: 24px;
}
.forum-container {
  max-width: 960px;
  margin: 0 auto;
}
.forum-header {
  text-align: center;
  padding: 32px 0 24px;
}
.forum-header h1 {
  font-size: 28px;
  font-weight: 700;
  color: #222;
  margin: 0 0 8px;
}
.forum-subtitle {
  color: #999;
  font-size: 14px;
  margin: 0;
}
.category-grid {
  display: flex;
  flex-direction: column;
  gap: 12px;
}
.category-card {
  display: flex;
  align-items: center;
  gap: 16px;
  padding: 20px 24px;
  background: #fff;
  border-radius: 12px;
  cursor: pointer;
  transition: all 0.2s;
  box-shadow: 0 1px 4px rgba(0,0,0,0.04);
}
.category-card:hover {
  transform: translateY(-1px);
  box-shadow: 0 4px 16px rgba(0,0,0,0.08);
  background: #fafcff;
}
.category-icon {
  width: 52px;
  height: 52px;
  border-radius: 14px;
  background: #e8f4fe;
  display: flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
}
.category-icon i {
  font-size: 24px;
  color: #00a1d6;
}
.category-info {
  flex: 1;
  min-width: 0;
}
.category-name {
  font-size: 17px;
  font-weight: 600;
  color: #222;
  margin: 0 0 4px;
}
.category-desc {
  font-size: 13px;
  color: #999;
  margin: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.category-stats {
  display: flex;
  gap: 20px;
  flex-shrink: 0;
}
.stat-item {
  text-align: center;
}
.stat-num {
  display: block;
  font-size: 16px;
  font-weight: 700;
  color: #333;
}
.stat-label {
  font-size: 11px;
  color: #bbb;
}
.category-arrow {
  color: #ddd;
  font-size: 14px;
  flex-shrink: 0;
  padding-left: 8px;
}
.forum-info-bar {
  display: flex;
  justify-content: center;
  gap: 32px;
  margin-top: 24px;
  padding: 16px;
  background: #fff;
  border-radius: 12px;
  box-shadow: 0 1px 4px rgba(0,0,0,0.04);
}
.info-item {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 13px;
  color: #666;
}
.info-item i {
  color: #00a1d6;
  font-size: 16px;
}
.info-item strong {
  color: #222;
}
@media (max-width: 767px) {
  .forum-page { padding: 16px; }
  .category-stats { display: none; }
  .category-card { padding: 16px; }
}
</style>
