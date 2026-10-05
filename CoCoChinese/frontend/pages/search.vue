<template>
  <div class="search-page container py-4">
    <div class="mb-4">
      <h4 v-if="query"><i class="fa fa-search me-2"></i>搜索 "{{ query }}"</h4>
      <h4 v-else><i class="fa fa-search me-2"></i>搜索</h4>
    </div>

    <div v-if="loading" class="text-center py-5">
      <i class="fa fa-spinner fa-spin fa-2x text-muted"></i>
      <p class="mt-2 text-muted">搜索中...</p>
    </div>

    <div v-else-if="error" class="alert alert-danger">{{ error }}</div>

    <div v-else-if="results.length" class="search-results">
      <p class="text-muted mb-3">共找到 {{ total }} 个结果</p>
      <div class="row g-3">
        <div v-for="v in results" :key="v.slug" class="col-md-6 col-lg-4">
          <div class="card h-100">
            <NuxtLink :to="'/player/' + v.slug">
              <img :src="v.image_url || '/images/videoImg.webp'" alt="" class="card-img-top" style="height:160px;object-fit:cover;">
            </NuxtLink>
            <div class="card-body">
              <h6 class="card-title">
                <NuxtLink :to="'/player/' + v.slug" class="text-decoration-none">{{ v.title }}</NuxtLink>
              </h6>
              <p class="card-text small text-muted search-desc" v-html="renderLinks(v.description || v.introduction || '')"></p>
              <div class="d-flex justify-content-between align-items-center">
                <small class="text-muted"><i class="fa fa-user-circle-o me-1"></i>{{ v.author }}</small>
                <small class="text-muted">{{ v.watch_volue || 0 }}播放</small>
              </div>
              <span v-if="v.video_type" class="badge bg-secondary mt-2">{{ v.video_type }}</span>
            </div>
          </div>
        </div>
      </div>

      <nav v-if="totalPages > 1" class="mt-4">
        <ul class="pagination justify-content-center">
          <li class="page-item" :class="{ disabled: page <= 1 }">
            <button class="page-link" @click="goPage(page - 1)">上一页</button>
          </li>
          <li v-for="p in pageRange" :key="p" class="page-item" :class="{ active: p === page }">
            <button class="page-link" @click="goPage(p)">{{ p }}</button>
          </li>
          <li class="page-item" :class="{ disabled: page >= totalPages }">
            <button class="page-link" @click="goPage(page + 1)">下一页</button>
          </li>
        </ul>
      </nav>
    </div>

    <div v-else class="empty-state text-center py-5">
      <i class="fa fa-search fa-3x text-muted mb-3"></i>
      <p class="text-muted">{{ query ? '没有找到相关结果' : '请输入搜索关键词' }}</p>
    </div>
  </div>
</template>

<script setup>
const route = useRoute()
const router = useRouter()
const { renderLinks } = useSafeLinks()

const query = ref(route.query.q || '')
const results = ref([])
const total = ref(0)
const page = ref(1)
const size = ref(20)
const loading = ref(false)
const error = ref('')

const totalPages = computed(() => Math.ceil(total.value / size.value) || 1)

const pageRange = computed(() => {
  const pages = []
  const tp = totalPages.value
  const cur = page.value
  let start = Math.max(1, cur - 2)
  let end = Math.min(tp, cur + 2)
  if (end - start < 4) {
    if (start === 1) end = Math.min(tp, start + 4)
    else start = Math.max(1, end - 4)
  }
  for (let i = start; i <= end; i++) pages.push(i)
  return pages
})

async function fetchResults() {
  const q = query.value.trim()
  if (!q) {
    results.value = []
    total.value = 0
    return
  }
  loading.value = true
  error.value = ''
  try {
    const res = await $fetch('/api/search', { params: { q, page: page.value, size: size.value } })
    if (res.success) {
      results.value = res.results || []
      total.value = res.total || 0
    } else {
      error.value = '搜索失败'
    }
  } catch (e) {
    error.value = '搜索失败，请稍后重试'
  } finally {
    loading.value = false
  }
}

function goPage(p) {
  if (p < 1 || p > totalPages.value) return
  page.value = p
  router.replace({ query: { ...route.query, page: p } })
  window.scrollTo({ top: 0, behavior: 'smooth' })
  fetchResults()
}

watch(() => route.query.q, (val) => {
  query.value = val || ''
  page.value = 1
  fetchResults()
})

fetchResults()
</script>

<style scoped>
.search-page { max-width: 1100px; margin: 0 auto; }
.card { transition: box-shadow 0.2s; }
.card:hover { box-shadow: 0 2px 12px rgba(0,0,0,0.1); }
.card-title a { color: #333; }
.card-title a:hover { color: #007bff; }
.search-desc :deep(.desc-link) { color: #1a73e8; text-decoration: none; }
.search-desc :deep(.desc-link:hover) { text-decoration: underline; }
.pagination { gap: 4px; }
.page-link { cursor: pointer; }
</style>
