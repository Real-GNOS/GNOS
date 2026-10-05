<template>
  <div class="upload-page container py-4">
    <h2 class="mb-4"><i class="fa fa-upload me-2"></i>投稿视频</h2>

    <div class="card">
      <div class="card-body">
        <form @submit.prevent="handleUpload">
          <div class="mb-4">
            <label class="form-label">视频文件</label>
            <div class="upload-zone text-center py-5 border rounded" @click="triggerFileInput" @dragover.prevent @drop.prevent="handleDrop">
              <i class="fa fa-cloud-upload fa-3x text-muted mb-3"></i>
              <p class="mb-1">点击或拖拽视频文件到此处</p>
              <p class="small text-muted">支持 MP4, FLV, AVI 格式，最大 4GB</p>
              <input ref="fileInput" type="file" accept="video/*" class="d-none" @change="handleFileChange">
              <div v-if="selectedFile" class="selected-file mt-3">
                <i class="fa fa-check-circle text-success me-2"></i>{{ selectedFile.name }}
              </div>
            </div>
          </div>

          <div class="mb-3">
            <label class="form-label">标题</label>
            <input v-model="title" type="text" class="form-control" placeholder="请输入视频标题" maxlength="80" required>
          </div>

          <div class="mb-3">
            <label class="form-label">简介 <small class="text-muted">（可粘贴链接，点击插入按钮可添加可点击超链接）</small></label>
            <textarea v-model="description" class="form-control" rows="4" placeholder="介绍一下你的视频" maxlength="1000"></textarea>
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
            <div class="col-md-4">
              <label class="form-label">分区</label>
              <select v-model="category" class="form-select">
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
            <div class="col-md-4">
              <label class="form-label">标签</label>
              <input v-model="tags" type="text" class="form-control" placeholder="用逗号分隔">
            </div>
            <div class="col-md-4">
              <label class="form-label">封面图</label>
              <input type="file" accept="image/*" class="form-control" @change="handleCoverChange">
            </div>
          </div>

          <div class="text-end">
            <button type="submit" class="btn btn-primary px-5" :disabled="submitting || isBanned">
              {{ submitting ? '提交中...' : '发布' }}
            </button>
          </div>
        </form>
      </div>
    </div>
  </div>
</template>

<script setup>
const title = ref('')
const description = ref('')
const category = ref('')
const tags = ref('')
const selectedFile = ref(null)
const coverFile = ref(null)
const fileInput = ref(null)
const submitting = ref(false)
const showLinkModal = ref(false)
const linkUrl = ref('')
const linkInputRef = ref(null)

function triggerFileInput() { fileInput.value?.click() }

function handleFileChange(e) {
  const file = e.target.files?.[0]
  if (file) selectedFile.value = file
}

function handleDrop(e) {
  const file = e.dataTransfer.files?.[0]
  if (file) selectedFile.value = file
}

function handleCoverChange(e) {
  const file = e.target.files?.[0]
  if (file) coverFile.value = file
}

function confirmInsertLink() {
  const url = linkUrl.value.trim()
  if (!url) return
  const fullUrl = url.match(/^https?:\/\//) ? url : 'https://' + url
  description.value = description.value + ' ' + fullUrl
  linkUrl.value = ''
  showLinkModal.value = false
}

watch(showLinkModal, (v) => {
  if (v) nextTick(() => linkInputRef.value?.focus())
})

async function handleUpload() {
  if (!selectedFile.value || !title.value) return
  submitting.value = true
  try {
    const fd = new FormData()
    fd.append('file', selectedFile.value)
    fd.append('title', title.value)
    fd.append('description', description.value)
    fd.append('category', category.value)
    fd.append('tags', tags.value)
    if (coverFile.value) fd.append('cover', coverFile.value)
    await $fetch('/api/upload', { method: 'POST', body: fd })
    navigateTo('/creator')
  } catch (e) {
    alert('投稿失败，请重试')
  } finally {
    submitting.value = false
  }
}
const { banned: isBanned } = useBan()
</script>

<style scoped>
.upload-page { max-width: 800px; margin: 0 auto; }
.upload-zone { border: 2px dashed #ddd; cursor: pointer; transition: border-color 0.3s; }
.upload-zone:hover { border-color: #007bff; background: #f8f9ff; }
.link-modal-overlay { position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: rgba(0,0,0,0.4); display: flex; align-items: center; justify-content: center; z-index: 9999; }
.link-modal { background: #fff; border-radius: 10px; padding: 1.5rem; width: 420px; max-width: 90vw; box-shadow: 0 4px 24px rgba(0,0,0,0.15); }
.link-modal h5 { margin-bottom: 1rem; }
</style>
