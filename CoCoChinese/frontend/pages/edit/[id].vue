<template>
  <div class="edit-page container py-4">
    <h2 class="mb-4"><i class="fa fa-pencil me-2"></i>编辑视频</h2>

    <div v-if="loading" class="text-center py-5">
      <i class="fa fa-spinner fa-spin fa-2x text-muted"></i>
      <p class="mt-2 text-muted">加载中...</p>
    </div>

    <div v-else-if="!video" class="text-center py-5">
      <p class="text-muted">视频不存在或无权编辑此视频</p>
      <NuxtLink to="/creator" class="btn btn-primary">返回创作中心</NuxtLink>
    </div>

    <div v-else class="card">
      <div class="card-body">
        <div v-if="video.video_url" class="mb-4">
          <label class="form-label">视频预览</label>
          <div class="preview-wrap">
            <video ref="previewRef" class="plyr" playsinline controls>
              <source :src="previewSrc" type="video/mp4">
            </video>
          </div>
        </div>

        <form @submit.prevent="handleSave">
          <div class="mb-3">
            <label class="form-label">视频文件 <small class="text-muted">(不选择则保留原文件)</small></label>
            <div class="d-flex align-items-center gap-3">
              <input ref="fileInput" type="file" accept="video/*" class="form-control" @change="handleFileChange">
              <span v-if="videoFile" class="text-success small"><i class="fa fa-check-circle"></i> {{ videoFile.name }}</span>
            </div>
          </div>

          <div class="mb-3">
            <label class="form-label">标题</label>
            <input v-model="form.title" type="text" class="form-control" maxlength="80" required>
          </div>

          <div class="mb-3">
            <label class="form-label">简介 <small class="text-muted">（可粘贴链接，点击插入按钮可添加可点击超链接）</small></label>
            <textarea ref="descRef" v-model="form.description" class="form-control" rows="4" maxlength="1000"></textarea>
            <div class="mt-1">
              <button type="button" class="btn btn-sm btn-outline-primary" @click="showLinkModal = true">
                <i class="fa fa-link me-1"></i>插入链接
              </button>
              <span class="ms-2 small text-muted">链接将通过中间页跳转</span>
            </div>
          </div>

          <div v-if="showLinkModal" class="link-modal-overlay" @click.self="showLinkModal = false">
            <div class="link-modal">
              <h5><i class="fa fa-link me-2"></i>插入超链接</h5>
              <div class="mb-3">
                <label class="form-label small">链接地址 (URL)</label>
                <input ref="linkInputRef" v-model="linkUrl" type="url" class="form-control" placeholder="https://example.com" @keydown.enter.prevent="confirmInsertLink">
              </div>
              <div class="d-flex gap-2 justify-content-end">
                <button type="button" class="btn btn-secondary btn-sm" @click="showLinkModal = false">取消</button>
                <button type="button" class="btn btn-primary btn-sm" @click="confirmInsertLink" :disabled="!linkUrl.trim()">插入</button>
              </div>
            </div>
          </div>

          <div class="row mb-3">
            <div class="col-md-6">
              <label class="form-label">分区</label>
              <select v-model="form.category" class="form-select">
                <option value="">请选择分区</option>
                <option value="动画">动画</option>
                <option value="音乐">音乐</option>
                <option value="游戏">游戏</option>
                <option value="生活">生活</option>
                <option value="知识">知识</option>
                <option value="科技">科技</option>
                <option value="舞蹈">舞蹈</option>
                <option value="美食">美食</option>
                <option value="时尚">时尚</option>
                <option value="娱乐">娱乐</option>
              </select>
            </div>
            <div class="col-md-6">
              <label class="form-label">标签</label>
              <input v-model="form.tags" type="text" class="form-control" placeholder="用逗号分隔">
            </div>
          </div>

          <div class="mb-3">
            <label class="form-label">封面图</label>
            <div class="d-flex align-items-center gap-3">
              <img v-if="coverPreview" :src="coverPreview" alt="" width="160" height="90" style="object-fit:cover;border-radius:4px;">
              <div>
                <input type="file" accept="image/*" class="form-control" @change="handleCoverChange">
                <small class="text-muted">不选择则保留原有封面</small>
              </div>
            </div>
          </div>

          <div class="d-flex gap-2 justify-content-end">
            <NuxtLink to="/creator" class="btn btn-secondary">取消</NuxtLink>
            <button type="submit" class="btn btn-primary px-4" :disabled="saving">
              {{ saving ? '保存中...' : '保存修改' }}
            </button>
          </div>
        </form>
      </div>
    </div>
  </div>
</template>

<script setup>
import 'plyr/dist/plyr.css'

const route = useRoute()
const video = ref(null)
const loading = ref(true)
const saving = ref(false)
const coverFile = ref(null)
const coverPreview = ref('')
const videoFile = ref(null)
const fileInput = ref(null)
const previewRef = ref(null)
const previewSrc = ref('')
const descRef = ref(null)
const showLinkModal = ref(false)
const linkUrl = ref('')
const linkInputRef = ref(null)

let player = null

const form = reactive({
  title: '',
  description: '',
  category: '',
  tags: '',
})

async function fetchVideo() {
  try {
    await $fetch(`/api/videos/${route.params.id}/owner-check`)
    const res = await $fetch(`/api/videos/${route.params.id}`)
    video.value = res
    form.title = res.title || ''
    form.description = res.introduction || ''
    form.category = res.video_type || ''
    form.tags = (Array.isArray(res.tags) ? res.tags : (res.tags || '').split(',').map(t => t.trim())).filter(Boolean).join(', ')
    coverPreview.value = res.image_url || ''
    previewSrc.value = res.video_url || ''
  } catch {
    video.value = null
    setTimeout(() => { navigateTo('/creator') }, 3000)
  } finally {
    loading.value = false
  }
}

function handleFileChange(e) {
  const file = e.target.files?.[0]
  if (file) {
    videoFile.value = file
    previewSrc.value = URL.createObjectURL(file)
  }
}

function handleCoverChange(e) {
  const file = e.target.files?.[0]
  if (file) {
    coverFile.value = file
    coverPreview.value = URL.createObjectURL(file)
  }
}

function confirmInsertLink() {
  const url = linkUrl.value.trim()
  if (!url) return
  const fullUrl = url.match(/^https?:\/\//) ? url : 'https://' + url
  form.description = form.description + ' ' + fullUrl
  linkUrl.value = ''
  showLinkModal.value = false
}

watch(showLinkModal, (v) => {
  if (v) nextTick(() => linkInputRef.value?.focus())
})

async function handleSave() {
  saving.value = true
  try {
    const fd = new FormData()
    fd.append('title', form.title)
    fd.append('description', form.description)
    fd.append('category', form.category)
    fd.append('tags', form.tags)
    if (coverFile.value) {
      fd.append('cover', coverFile.value)
    }
    if (videoFile.value) {
      fd.append('file', videoFile.value)
    }
    await $fetch(`/api/videos/${route.params.id}`, {
      method: 'PUT',
      body: fd,
    })
    navigateTo('/creator')
  } catch {
    alert('保存失败，请重试')
  } finally {
    saving.value = false
  }
}

onMounted(async () => {
  await fetchVideo()
  await nextTick()
  if (previewRef.value) {
    const Plyr = (await import('plyr')).default
    player = new Plyr(previewRef.value, { controls: ['play-large', 'play', 'progress', 'current-time', 'volume', 'fullscreen'] })
  }
})
</script>

<style scoped>
.edit-page { max-width: 900px; margin: 0 auto; }
.preview-wrap { max-width: 100%; border-radius: 8px; overflow: hidden; background: #000; }
.preview-wrap video { width: 100%; display: block; }
.link-modal-overlay { position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: rgba(0,0,0,0.4); display: flex; align-items: center; justify-content: center; z-index: 9999; }
.link-modal { background: #fff; border-radius: 10px; padding: 1.5rem; width: 420px; max-width: 90vw; box-shadow: 0 4px 24px rgba(0,0,0,0.15); }
.link-modal h5 { margin-bottom: 1rem; }
</style>