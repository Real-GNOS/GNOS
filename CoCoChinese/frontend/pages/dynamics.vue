<template>
  <div class="dynamics-page container py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
      <h2 class="mb-0"><i class="fa fa-rss me-2"></i>好友动态</h2>
      <button class="btn btn-outline-primary btn-sm" @click="showPost = true"><i class="fa fa-plus me-1"></i>发布动态</button>
    </div>

    <div class="dynamics-feed">
      <div v-for="act in activities" :key="act.id" class="card mb-3">
        <div class="card-body">
          <div class="d-flex gap-3">
            <NuxtLink to="/user/center">
              <img :src="act.avatar_url || '/images/authorImg.webp'" alt="" width="48" height="48" class="rounded-circle">
            </NuxtLink>
            <div class="flex-grow-1">
              <div class="d-flex justify-content-between">
                <div>
                  <strong>{{ act.display_name || act.username }}</strong>
                  <span class="badge bg-secondary ms-2">{{ act.type }}</span>
                </div>
                <small class="text-muted">{{ formatTime(act.created_at) }}</small>
              </div>
              <p class="mt-2 mb-0">{{ act.content }}</p>
            </div>
          </div>
        </div>
      </div>
    </div>

    <div v-if="!activities.length" class="empty-state text-center py-5">
      <i class="fa fa-users fa-3x text-muted mb-3"></i>
      <p class="text-muted">暂无动态</p>
    </div>

    <div v-if="showPost" class="modal-backdrop" @click.self="showPost = false">
      <div class="modal-content" @click.stop>
        <div class="modal-header"><h5>发布动态</h5><button class="btn-close" @click="showPost = false"></button></div>
        <div class="modal-body">
          <div class="mb-3">
            <textarea v-model="postContent" class="form-control" rows="4" placeholder="说点什么..." maxlength="500"></textarea>
          </div>
        </div>
        <div class="modal-footer">
              <button class="btn btn-primary" @click="submitPost" :disabled="!postContent.trim() || isBanned">发布</button>
          <button class="btn btn-secondary" @click="showPost = false">取消</button>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
const activities = ref([])
const showPost = ref(false)
const postContent = ref('')

async function fetchDynamics() {
  try {
    const res = await $fetch('/api/dynamics')
    if (res.success) activities.value = res.data
  } catch {
  }
}

async function submitPost() {
  if (!postContent.value.trim()) return
  try {
    const res = await $fetch('/api/dynamics', { method: 'POST', body: { type: '动态', content: postContent.value } })
    if (res.success) {
      showPost.value = false
      postContent.value = ''
      fetchDynamics()
    }
  } catch {
    alert('发布失败')
  }
}

function formatTime(t) {
  return new Date(t).toLocaleString()
}

fetchDynamics()
const { banned: isBanned } = useBan()
</script>

<style scoped>
.dynamics-page { max-width: 700px; margin: 0 auto; }
.modal-backdrop { position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.5); z-index: 1050; display: flex; align-items: center; justify-content: center; }
.modal-content { background: #fff; border-radius: 8px; width: 90%; max-width: 500px; }
.modal-header, .modal-footer { padding: 1rem; border-bottom: 1px solid #eee; }
.modal-footer { border-top: 1px solid #eee; border-bottom: none; }
.modal-body { padding: 1rem; }
</style>
