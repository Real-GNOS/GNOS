<template>
  <div class="history-page container py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
      <h2 class="mb-0"><i class="fa fa-history me-2"></i>观看历史</h2>
      <button class="btn btn-outline-danger btn-sm" @click="clearAll" :disabled="!history.length"><i class="fa fa-trash me-1"></i>清空历史</button>
    </div>

    <div class="list-group">
      <div v-for="item in history" :key="item.id" class="list-group-item list-group-item-action d-flex gap-3 align-items-center">
        <NuxtLink :to="'/player/' + item.slug" class="d-flex gap-3 text-decoration-none flex-grow-1">
          <img :src="item.image_url || '/images/videoImg.webp'" alt="" width="160" height="90" style="object-fit:cover;border-radius:4px;">
          <div class="flex-grow-1">
            <h6 class="mb-1">{{ item.title }}</h6>
            <p class="mb-1 small text-muted">{{ item.author }} · {{ item.video_type }} · {{ item.video_time }}</p>
              <div class="d-flex align-items-center gap-2">
                <div class="progress flex-grow-1" style="height:4px;">
                  <div class="progress-bar" :style="{ width: calcProgress(item) + '%' }"></div>
                </div>
                <small class="text-muted">{{ Math.round(calcProgress(item)) }}%</small>
              </div>
            <small class="text-muted">{{ new Date(item.watched_at).toLocaleString() }}</small>
          </div>
        </NuxtLink>
        <button class="btn btn-sm btn-outline-secondary" @click="removeItem(item.video_id)" title="移出历史">
          <i class="fa fa-times"></i>
        </button>
      </div>
    </div>

    <div v-if="!history.length" class="empty-state text-center py-5">
      <i class="fa fa-clock-o fa-3x text-muted mb-3"></i>
      <p class="text-muted">暂无观看记录</p>
      <NuxtLink to="/" class="btn btn-primary">去观看视频</NuxtLink>
    </div>
  </div>
</template>

<script setup>
const history = ref([])

function parseVideoTime(t) {
  if (!t) return 0
  const parts = t.split(':').map(Number)
  if (parts.length === 2) return parts[0] * 60 + parts[1]
  if (parts.length === 3) return parts[0] * 3600 + parts[1] * 60 + parts[2]
  return 0
}

function calcProgress(item) {
  const total = parseVideoTime(item.video_time)
  if (!total || !item.progress) return 0
  return Math.min(Math.round((item.progress / total) * 100), 100)
}

async function fetchHistory() {
  try {
    const res = await $fetch('/api/history')
    if (res.success) history.value = res.data
  } catch {
  }
}

async function removeItem(videoId) {
  try {
    await $fetch('/api/history', { method: 'POST', body: { video_id: videoId } })
    history.value = history.value.filter(h => h.video_id !== videoId)
  } catch {
  }
}

async function clearAll() {
  if (!confirm('确定清空观看历史？')) return
  try {
    await $fetch('/api/history/clear', { method: 'POST' })
    history.value = []
  } catch {
    alert('清空失败')
  }
}

fetchHistory()
</script>

<style scoped>
.history-page { max-width: 900px; margin: 0 auto; }
.progress { background: #eee; }
</style>
