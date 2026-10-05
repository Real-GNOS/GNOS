<template>
  <div class="forum-page">
    <div class="forum-container">
      <div class="breadcrumb-bar">
        <NuxtLink to="/forum">论坛</NuxtLink>
        <i class="fa fa-chevron-right sep"></i>
        <span>{{ category?.name || '加载中...' }}</span>
      </div>

      <div class="category-head" v-if="category">
        <div class="ch-left">
          <div class="ch-icon"><i :class="'fa ' + (category.icon || 'fa-comments')"></i></div>
          <div>
            <h2>{{ category.name }}</h2>
            <p>{{ category.description }}</p>
          </div>
        </div>
        <NuxtLink to="/forum/create" class="btn-new-topic">
          <i class="fa fa-pencil"></i> 发布话题
        </NuxtLink>
      </div>

      <div class="topic-controls" v-if="topics.length">
        <span class="topic-count">共 {{ total }} 个话题</span>
      </div>

      <div class="topic-list">
        <div v-for="topic in topics" :key="topic.id" class="topic-item" @click="navigateTo(`/forum/topic/${topic.slug}`)">
          <div class="topic-icon">
            <i v-if="topic.is_pinned" class="fa fa-thumb-tack pinned-icon"></i>
            <i v-else class="fa fa-comment-o"></i>
          </div>
          <div class="topic-info">
            <div class="topic-title-row">
              <span v-if="topic.is_pinned" class="pin-badge">置顶</span>
              <span v-if="topic.is_locked" class="lock-badge">已锁</span>
              <h3 class="topic-title">{{ topic.title }}</h3>
            </div>
            <div class="topic-meta">
              <span class="topic-author">
                <img :src="topic.author_avatar || '/images/authorImg.webp'" class="mini-avatar">
                {{ topic.author_name }}
              </span>
              <span class="meta-sep">·</span>
              <span class="topic-time">{{ timeAgo(topic.created_at) }}</span>
            </div>
          </div>
          <div class="topic-stats">
            <div class="ts-item">
              <i class="fa fa-eye"></i>
              <span>{{ topic.view_count }}</span>
            </div>
            <div class="ts-item">
              <i class="fa fa-reply"></i>
              <span>{{ topic.reply_count }}</span>
            </div>
          </div>
          <div class="topic-last" v-if="topic.last_post_at">
            <small>{{ timeAgo(topic.last_post_at) }}</small>
          </div>
        </div>
        <div v-if="!topics.length" class="empty-state">
          <i class="fa fa-inbox fa-3x text-muted mb-2"></i>
          <p>暂无话题，快来发布第一个话题吧</p>
        </div>
      </div>

      <div class="pagination" v-if="totalPages > 1">
        <button :disabled="page <= 1" @click="changePage(page - 1)">上一页</button>
        <span class="page-info">{{ page }} / {{ totalPages }}</span>
        <button :disabled="page >= totalPages" @click="changePage(page + 1)">下一页</button>
      </div>
    </div>
  </div>
</template>

<script setup>
const route = useRoute()
const slug = computed(() => route.params.slug)
const page = ref(parseInt(route.query.page) || 1)
const pageSize = 20

const { data: catData } = await useFetch(() => `/api/forum/categories`)
const category = computed(() => {
  const cats = catData.value?.data || []
  return cats.find(c => c.slug === slug.value)
})

const { data: topicData, refresh } = await useFetch(() => `/api/forum/topics?category=${slug.value}&page=${page.value}&pageSize=${pageSize}`)
const topics = computed(() => topicData.value?.data || [])
const total = computed(() => topicData.value?.total || 0)
const totalPages = computed(() => Math.ceil(total.value / pageSize) || 1)

function changePage(p) {
  page.value = p
  refresh()
}

function timeAgo(dateStr) {
  if (!dateStr) return ''
  const diff = Date.now() - new Date(dateStr).getTime()
  const mins = Math.floor(diff / 60000)
  if (mins < 1) return '刚刚'
  if (mins < 60) return `${mins}分钟前`
  const hours = Math.floor(mins / 60)
  if (hours < 24) return `${hours}小时前`
  const days = Math.floor(hours / 24)
  if (days < 30) return `${days}天前`
  return new Date(dateStr).toLocaleDateString('zh-CN')
}
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
.breadcrumb-bar {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
  color: #999;
  margin-bottom: 16px;
}
.breadcrumb-bar a { color: #00a1d6; text-decoration: none; }
.breadcrumb-bar .sep { font-size: 10px; color: #ccc; }
.category-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 20px 24px;
  background: #fff;
  border-radius: 12px;
  margin-bottom: 16px;
  box-shadow: 0 1px 4px rgba(0,0,0,0.04);
}
.ch-left {
  display: flex;
  align-items: center;
  gap: 16px;
}
.ch-icon {
  width: 48px;
  height: 48px;
  border-radius: 12px;
  background: #e8f4fe;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 22px;
  color: #00a1d6;
}
.category-head h2 {
  font-size: 20px;
  font-weight: 700;
  margin: 0 0 4px;
  color: #222;
}
.category-head p {
  font-size: 13px;
  color: #999;
  margin: 0;
}
.btn-new-topic {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 10px 20px;
  background: #00a1d6;
  color: #fff;
  border-radius: 8px;
  text-decoration: none;
  font-size: 14px;
  font-weight: 500;
  transition: background 0.2s;
  flex-shrink: 0;
}
.btn-new-topic:hover { background: #0088b3; color: #fff; }
.topic-controls {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 8px;
  padding: 0 4px;
}
.topic-count { font-size: 13px; color: #999; }
.topic-list {
  background: #fff;
  border-radius: 12px;
  overflow: hidden;
  box-shadow: 0 1px 4px rgba(0,0,0,0.04);
}
.topic-item {
  display: flex;
  align-items: center;
  gap: 14px;
  padding: 16px 20px;
  cursor: pointer;
  transition: background 0.15s;
  border-bottom: 1px solid #f5f5f5;
}
.topic-item:last-child { border-bottom: none; }
.topic-item:hover { background: #f7f8fa; }
.topic-icon {
  width: 36px;
  height: 36px;
  border-radius: 50%;
  background: #f0f4f8;
  display: flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
  color: #00a1d6;
  font-size: 16px;
}
.pinned-icon { color: #f39c12; }
.topic-info { flex: 1; min-width: 0; }
.topic-title-row {
  display: flex;
  align-items: center;
  gap: 6px;
  margin-bottom: 4px;
}
.pin-badge, .lock-badge {
  font-size: 10px;
  padding: 1px 6px;
  border-radius: 4px;
  font-weight: 600;
  flex-shrink: 0;
}
.pin-badge { background: #fef9e7; color: #e67e22; }
.lock-badge { background: #fce4ec; color: #e74c3c; }
.topic-title {
  font-size: 15px;
  font-weight: 600;
  color: #222;
  margin: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.topic-meta {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
  color: #999;
}
.mini-avatar {
  width: 18px;
  height: 18px;
  border-radius: 50%;
  vertical-align: middle;
  margin-right: 2px;
}
.meta-sep { color: #ddd; }
.topic-stats {
  display: flex;
  gap: 16px;
  flex-shrink: 0;
  color: #999;
  font-size: 13px;
}
.ts-item { display: flex; align-items: center; gap: 4px; }
.ts-item i { font-size: 13px; }
.topic-last {
  flex-shrink: 0;
  color: #bbb;
  font-size: 12px;
  min-width: 60px;
  text-align: right;
}
.empty-state {
  text-align: center;
  padding: 60px 20px;
  color: #999;
}
.pagination {
  display: flex;
  justify-content: center;
  align-items: center;
  gap: 12px;
  margin-top: 20px;
}
.pagination button {
  padding: 8px 16px;
  border: 1px solid #ddd;
  background: #fff;
  border-radius: 8px;
  cursor: pointer;
  font-size: 13px;
  color: #333;
  transition: all 0.15s;
}
.pagination button:hover:not(:disabled) { border-color: #00a1d6; color: #00a1d6; }
.pagination button:disabled { opacity: 0.5; cursor: not-allowed; }
.page-info { font-size: 13px; color: #999; }
@media (max-width: 767px) {
  .forum-page { padding: 16px; }
  .topic-stats, .topic-last { display: none; }
}
</style>
