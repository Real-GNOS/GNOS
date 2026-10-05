<template>
  <div class="creator-page container py-4">
    <div class="row mb-4">
      <div class="col-12">
        <h2><i class="fa fa-lightbulb-o me-2"></i>创作中心</h2>
      </div>
    </div>

    <div class="row mb-4">
      <div v-for="stat in statItems" :key="stat.label" class="col-md-3 col-6 mb-3">
        <div class="card text-center">
          <div class="card-body">
            <div class="stat-number text-primary">{{ stat.value }}</div>
            <div class="stat-label text-muted">{{ stat.label }}</div>
          </div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-header d-flex justify-content-between align-items-center">
        <span><i class="fa fa-video-camera me-2"></i>我的投稿 ({{ myVideos.length }})</span>
        <NuxtLink to="/upload" class="btn btn-primary btn-sm"><i class="fa fa-upload me-1"></i>投稿</NuxtLink>
      </div>
      <div class="card-body p-0">
          <div v-for="v in myVideos" :key="v.id" class="d-flex gap-3 p-3 border-bottom">
            <NuxtLink :to="'/player/' + v.slug">
              <img :src="v.image_url || '/images/videoImg.webp'" alt="" width="120" height="68" style="object-fit:cover;border-radius:4px;">
            </NuxtLink>
            <div class="flex-grow-1">
              <h6 class="mb-1"><NuxtLink :to="'/player/' + v.slug" class="text-decoration-none">{{ v.title }}</NuxtLink></h6>
              <p class="mb-1 small text-muted">{{ v.video_type }} · {{ v.watch_volue || 0 }}播放 · {{ v.like_volue || 0 }}赞</p>
              <small class="text-muted">{{ new Date(v.created_at).toLocaleDateString() }}</small>
            </div>
            <div class="d-flex align-items-center">
              <NuxtLink :to="'/edit/' + v.id" class="btn btn-outline-primary btn-sm">
                <i class="fa fa-pencil"></i>
              </NuxtLink>
            </div>
          </div>
        <div v-if="!myVideos.length" class="empty-state text-center py-5">
          <i class="fa fa-film fa-3x text-muted mb-3"></i>
          <p class="text-muted">还没有投稿视频</p>
          <NuxtLink to="/upload" class="btn btn-primary">立即投稿</NuxtLink>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
const creatorStats = ref({ total_views: 0, total_likes: 0, total_videos: 0, followers: 0 })
const myVideos = ref([])

const statItems = computed(() => [
  { label: '总播放', value: creatorStats.value.total_views },
  { label: '总获赞', value: creatorStats.value.total_likes },
  { label: '粉丝', value: creatorStats.value.followers },
  { label: '总投稿', value: creatorStats.value.total_videos },
])

async function fetchData() {
  try {
    const userRes = await $fetch('/api/user/me', { default: () => null, transform: r => r.user || null })
    if (!userRes) { navigateTo('/login'); return }

    const [statsRes, vidRes] = await Promise.all([
      $fetch('/api/creator/stats'),
      $fetch('/api/creator/videos'),
    ])
    if (statsRes.success) creatorStats.value = statsRes.data
    if (vidRes.success) myVideos.value = vidRes.rows || []
    else myVideos.value = []
  } catch (e) {
    if (e?.statusCode === 401) {
      navigateTo('/login')
    }
  }
}

fetchData()
</script>

<style scoped>
.creator-page { max-width: 1000px; margin: 0 auto; }
.stat-number { font-size: 24px; font-weight: 700; }
.stat-label { font-size: 13px; }
</style>
