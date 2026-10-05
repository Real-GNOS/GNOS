<template>
  <div class="article-page container py-4">
    <div v-if="loading" class="text-center py-5">
      <i class="fa fa-spinner fa-spin fa-2x text-muted"></i>
    </div>

    <div v-else-if="!article" class="text-center py-5">
      <p class="text-muted">文章不存在</p>
      <p v-if="errorMsg" class="text-danger small">{{ errorMsg }}</p>
      <NuxtLink to="/articles" class="btn btn-primary">返回文章列表</NuxtLink>
    </div>

    <article v-else class="article-content">
      <div class="article-header">
        <h1 class="article-title">{{ article.title }}</h1>
        <div class="article-meta">
          <span class="meta-item"><i class="fa fa-user me-1"></i>{{ article.author }}</span>
          <span v-if="article.category" class="meta-item"><i class="fa fa-folder me-1"></i>{{ article.category }}</span>
          <span class="meta-item"><i class="fa fa-clock-o me-1"></i>{{ new Date(article.created_at).toLocaleDateString() }}</span>
          <span class="meta-item"><i class="fa fa-eye me-1"></i>{{ article.view_count || 0 }} 阅读</span>
        </div>
        <div v-if="article.tags?.length" class="article-tags">
          <span v-for="tag in article.tags" :key="tag" class="badge bg-light text-dark me-1">{{ tag }}</span>
        </div>
        <div v-if="isOwner" class="mt-3">
          <NuxtLink :to="'/edit/article/' + article.slug" class="btn btn-outline-primary btn-sm"><i class="fa fa-pencil me-1"></i>编辑</NuxtLink>
        </div>
      </div>

      <div class="article-body" v-html="article.content"></div>
    </article>
  </div>
</template>

<script setup>
const route = useRoute()
const article = ref(null)
const loading = ref(true)
const user = ref(null)
const errorMsg = ref('')

const isOwner = computed(() => user.value && article.value && user.value.username === article.value.author)

async function fetchArticle() {
  try {
    const res = await $fetch(`/api/articles/${route.params.slug}`)
    if (res.success) article.value = res.data
  } catch {} finally {
    loading.value = false
  }
}

onMounted(async () => {
  try {
    const me = await $fetch('/api/user/me', { default: () => null, transform: r => r.user || null })
    if (me) user.value = me
  } catch {}
  await fetchArticle()
})
</script>

<style scoped>
.article-page { max-width: 800px; margin: 0 auto; }
.article-content { background: #fff; border-radius: 12px; padding: 40px; box-shadow: 0 2px 12px rgba(0,0,0,0.06); }
.article-header { margin-bottom: 32px; }
.article-title { font-size: 28px; font-weight: 700; color: #222; margin-bottom: 16px; line-height: 1.4; }
.article-meta { display: flex; flex-wrap: wrap; gap: 16px; margin-bottom: 12px; }
.meta-item { font-size: 13px; color: #999; }
.article-tags { margin-bottom: 8px; }
.article-cover { margin-bottom: 32px; border-radius: 12px; overflow: hidden; }
.article-cover img { width: 100%; max-height: 400px; object-fit: cover; display: block; }
.article-body { font-size: 16px; line-height: 1.9; color: #333; }
.article-body :deep(p) { margin: 0 0 16px; }
.article-body :deep(h2) { margin: 32px 0 16px; font-size: 24px; color: #222; }
.article-body :deep(h3) { margin: 28px 0 12px; font-size: 20px; color: #222; }
.article-body :deep(img) { max-width: 100%; border-radius: 8px; margin: 16px 0; display: block; }
.article-body :deep(blockquote) {
  border-left: 4px solid #00a1d6; padding: 12px 20px; margin: 16px 0;
  background: #f8f9fa; border-radius: 0 8px 8px 0; color: #666;
}
.article-body :deep(ul), .article-body :deep(ol) { padding-left: 24px; margin-bottom: 16px; }
.article-body :deep(li) { margin-bottom: 4px; }
.article-body :deep(a) { color: #00a1d6; text-decoration: underline; }
.article-body :deep(blockquote p) { margin: 0; }
</style>
