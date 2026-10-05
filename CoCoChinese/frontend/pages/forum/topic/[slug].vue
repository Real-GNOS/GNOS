<template>
  <div class="forum-page">
    <div class="forum-container">
      <div class="breadcrumb-bar">
        <NuxtLink to="/forum">论坛</NuxtLink>
        <i class="fa fa-chevron-right sep"></i>
        <NuxtLink :to="`/forum/category/${topic?.category_slug}`" v-if="topic">{{ topic.category_name }}</NuxtLink>
        <i class="fa fa-chevron-right sep" v-if="topic"></i>
        <span>{{ topic?.title || '加载中...' }}</span>
      </div>

      <div v-if="topic" class="topic-head">
        <h1 class="topic-title">{{ topic.title }}</h1>
        <div class="topic-head-meta">
          <span class="thm-item">
            <img :src="topic.author_avatar || '/images/authorImg.webp'" class="thm-avatar">
            {{ topic.author_name }}
          </span>
          <span class="thm-item"><i class="fa fa-clock-o"></i> {{ timeAgo(topic.created_at) }}</span>
          <span class="thm-item"><i class="fa fa-eye"></i> {{ topic.view_count }}</span>
          <span class="thm-item"><i class="fa fa-reply"></i> {{ topic.reply_count }}</span>
        </div>
      </div>

      <div class="original-post" v-if="topic">
        <div class="post-sidebar">
          <img :src="topic.author_avatar || '/images/authorImg.webp'" class="post-avatar">
          <div class="post-author-name">{{ topic.author_name }}</div>
        </div>
        <div class="post-body">
          <div class="post-content" v-html="renderedContent"></div>
        </div>
      </div>

      <div class="replies-section" v-if="posts.length">
        <h3 class="replies-title">回复 ({{ total }})</h3>
        <div v-for="post in posts" :key="post.id" class="reply-item">
          <div class="post-sidebar">
            <img :src="post.author_avatar || '/images/authorImg.webp'" class="post-avatar">
            <div class="post-author-name">{{ post.author_name }}</div>
          </div>
          <div class="post-body">
            <div class="post-meta">
              <span class="post-time">{{ timeAgo(post.created_at) }}</span>
            </div>
            <div class="post-content">{{ post.content }}</div>
          </div>
        </div>

        <div class="pagination" v-if="totalPages > 1">
          <button :disabled="page <= 1" @click="changePage(page - 1)">上一页</button>
          <span class="page-info">{{ page }} / {{ totalPages }}</span>
          <button :disabled="page >= totalPages" @click="changePage(page + 1)">下一页</button>
        </div>
      </div>

      <div v-if="!topic?.is_locked" class="reply-form">
        <h4>发表回复</h4>
        <textarea v-model="replyContent" placeholder="写下你的回复..." rows="4" maxlength="10000"></textarea>
        <div class="reply-form-actions">
          <span class="char-count">{{ replyContent.length }} / 10000</span>
          <button class="btn-reply" @click="submitReply" :disabled="!replyContent.trim() || submitting">
            {{ submitting ? '提交中...' : '发表回复' }}
          </button>
        </div>
      </div>
      <div v-else class="locked-notice">
        <i class="fa fa-lock"></i> 此话题已被锁定，无法回复
      </div>
    </div>
  </div>
</template>

<script setup>
const route = useRoute()
const slug = computed(() => route.params.slug)
const page = ref(parseInt(route.query.page) || 1)
const pageSize = 20
const replyContent = ref('')
const submitting = ref(false)
const { user: me } = useUser()

const { data: topicData, refresh } = await useFetch(() => `/api/forum/topics/${slug.value}?page=${page.value}&pageSize=${pageSize}`)
const topic = computed(() => topicData.value?.data?.topic || null)
const posts = computed(() => topicData.value?.data?.posts || [])
const total = computed(() => topicData.value?.total || 0)
const totalPages = computed(() => Math.ceil(total.value / pageSize) || 1)

const renderedContent = computed(() => {
  if (!topic.value?.content) return ''
  return topic.value.content.replace(/\n/g, '<br>')
})

function changePage(p) {
  page.value = p
  refresh()
  window.scrollTo({ top: 0, behavior: 'smooth' })
}

async function submitReply() {
  if (!replyContent.value.trim() || submitting.value) return
  if (!me.value) { alert('请先登录'); return }
  submitting.value = true
  try {
    const res = await $fetch('/api/forum/posts', {
      method: 'POST',
      body: { topic_id: topic.value.id, content: replyContent.value.trim() },
    })
    if (res.success) {
      replyContent.value = ''
      page.value = 1
      await refresh()
      setTimeout(() => window.scrollTo({ top: document.body.scrollHeight, behavior: 'smooth' }), 100)
    }
  } catch (e) {
    alert(e.data?.message || '回复失败')
  }
  submitting.value = false
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
  max-width: 860px;
  margin: 0 auto;
}
.breadcrumb-bar {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
  color: #999;
  margin-bottom: 16px;
  flex-wrap: wrap;
}
.breadcrumb-bar a { color: #00a1d6; text-decoration: none; }
.breadcrumb-bar .sep { font-size: 10px; color: #ccc; }
.topic-head {
  padding: 24px 28px;
  background: #fff;
  border-radius: 12px 12px 0 0;
  box-shadow: 0 1px 4px rgba(0,0,0,0.04);
}
.topic-head h1 {
  font-size: 22px;
  font-weight: 700;
  margin: 0 0 12px;
  color: #222;
}
.topic-head-meta {
  display: flex;
  align-items: center;
  gap: 16px;
  font-size: 13px;
  color: #999;
  flex-wrap: wrap;
}
.thm-item { display: flex; align-items: center; gap: 4px; }
.thm-avatar {
  width: 24px; height: 24px; border-radius: 50%;
  object-fit: cover;
}
.original-post {
  display: flex;
  gap: 20px;
  padding: 24px 28px;
  background: #fff;
  border-top: 1px solid #f0f0f0;
  box-shadow: 0 1px 4px rgba(0,0,0,0.04);
}
.post-sidebar {
  width: 80px;
  flex-shrink: 0;
  text-align: center;
}
.post-avatar {
  width: 56px;
  height: 56px;
  border-radius: 50%;
  object-fit: cover;
  margin-bottom: 6px;
}
.post-author-name {
  font-size: 12px;
  font-weight: 600;
  color: #333;
  word-break: break-all;
}
.post-body { flex: 1; min-width: 0; }
.post-content {
  font-size: 15px;
  line-height: 1.7;
  color: #333;
  word-break: break-word;
}
.post-meta {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 10px;
}
.post-time { font-size: 12px; color: #bbb; }
.replies-section {
  background: #fff;
  border-top: 1px solid #f0f0f0;
  box-shadow: 0 1px 4px rgba(0,0,0,0.04);
}
.replies-title {
  padding: 16px 28px;
  margin: 0;
  font-size: 16px;
  font-weight: 600;
  color: #222;
  border-bottom: 1px solid #f0f0f0;
}
.reply-item {
  display: flex;
  gap: 20px;
  padding: 20px 28px;
  border-bottom: 1px solid #f5f5f5;
}
.reply-item:last-child { border-bottom: none; }
.pagination {
  display: flex;
  justify-content: center;
  align-items: center;
  gap: 12px;
  padding: 16px;
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
.reply-form {
  margin-top: 16px;
  padding: 24px 28px;
  background: #fff;
  border-radius: 12px;
  box-shadow: 0 1px 4px rgba(0,0,0,0.04);
}
.reply-form h4 {
  font-size: 16px;
  font-weight: 600;
  margin: 0 0 12px;
  color: #222;
}
.reply-form textarea {
  width: 100%;
  padding: 12px;
  border: 1px solid #e8e8e8;
  border-radius: 8px;
  font-size: 14px;
  line-height: 1.6;
  resize: vertical;
  outline: none;
  transition: border-color 0.2s;
  box-sizing: border-box;
  font-family: inherit;
}
.reply-form textarea:focus { border-color: #00a1d6; }
.reply-form-actions {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-top: 12px;
}
.char-count { font-size: 12px; color: #bbb; }
.btn-reply {
  padding: 10px 24px;
  background: #00a1d6;
  color: #fff;
  border: none;
  border-radius: 8px;
  font-size: 14px;
  font-weight: 500;
  cursor: pointer;
  transition: background 0.2s;
}
.btn-reply:hover { background: #0088b3; }
.btn-reply:disabled { background: #ccc; cursor: not-allowed; }
.locked-notice {
  margin-top: 16px;
  padding: 16px;
  background: #fef9e7;
  border-radius: 8px;
  text-align: center;
  color: #e67e22;
  font-size: 14px;
}
@media (max-width: 767px) {
  .forum-page { padding: 16px; }
  .post-sidebar { width: 60px; }
  .post-avatar { width: 40px; height: 40px; }
  .original-post, .reply-item { padding: 16px; gap: 12px; }
  .topic-head { padding: 16px; }
}
</style>
