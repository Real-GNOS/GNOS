<template>
  <div class="articles-page container py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
      <h2 class="mb-0"><i class="fa fa-file-text me-2"></i>文章</h2>
      <NuxtLink to="/create/article" class="btn btn-primary"><i class="fa fa-pencil me-1"></i>写文章</NuxtLink>
    </div>

    <div v-if="loading" class="text-center py-5">
      <i class="fa fa-spinner fa-spin fa-2x text-muted"></i>
    </div>

    <template v-else>
      <div v-if="!articles.length" class="empty-state text-center py-5">
        <i class="fa fa-file-text-o fa-3x text-muted mb-3"></i>
        <p class="text-muted">还没有文章</p>
        <NuxtLink to="/create/article" class="btn btn-primary">写第一篇文章</NuxtLink>
      </div>

      <div class="article-grid">
        <div v-for="item in articles" :key="item.id" class="article-card card">
          <NuxtLink :to="'/articles/' + item.slug" class="text-decoration-none">
            <div v-if="item.cover_image" class="article-cover">
              <img :src="item.cover_image" :alt="item.title">
            </div>
            <div class="card-body">
              <h5 class="article-title">{{ item.title }}</h5>
              <p class="article-meta text-muted small mb-2">
                {{ item.author }} · {{ item.category }} · {{ new Date(item.created_at).toLocaleDateString() }}
              </p>
              <p class="article-summary text-muted">{{ stripHtml(item.content).slice(0, 120) }}</p>
              <div class="d-flex gap-2 flex-wrap">
                <span v-for="tag in (item.tags || [])" :key="tag" class="badge bg-light text-dark">{{ tag }}</span>
              </div>
            </div>
          </NuxtLink>
        </div>
      </div>
    </template>
  </div>
</template>

<script setup>
const articles = ref([])
const loading = ref(true)

function stripHtml(html) {
  if (!html) return ''
  const div = document.createElement('div')
  div.innerHTML = html
  return div.textContent || div.innerText || ''
}

async function fetchArticles() {
  try {
    const res = await $fetch('/api/articles')
    if (res.success) articles.value = res.data
  } catch {} finally {
    loading.value = false
  }
}

fetchArticles()
</script>

<style scoped>
.articles-page { max-width: 1000px; margin: 0 auto; }
.article-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 20px; }
.article-card { transition: transform 0.2s, box-shadow 0.2s; overflow: hidden; }
.article-card:hover { transform: translateY(-2px); box-shadow: 0 4px 16px rgba(0,0,0,0.1); }
.article-cover { width: 100%; height: 180px; overflow: hidden; }
.article-cover img { width: 100%; height: 100%; object-fit: cover; transition: transform 0.3s; }
.article-card:hover .article-cover img { transform: scale(1.05); }
.article-title { font-size: 18px; font-weight: 600; color: #222; margin-bottom: 6px; }
.article-meta { font-size: 12px; }
.article-summary { font-size: 13px; line-height: 1.5; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden; }
.empty-state p { margin: 0; }
</style>
