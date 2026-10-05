<template>
  <div class="gallery-creator container py-4">
    <h2 class="mb-4">创建图集</h2>
    <div class="card">
      <div class="card-body">
        <form @submit.prevent="handleCreate">
          <div class="mb-3">
            <label class="form-label">标题</label>
            <input v-model="title" type="text" class="form-control form-control-lg" maxlength="200" required placeholder="输入图集标题">
          </div>

          <div class="mb-3">
            <label class="form-label">描述 <small class="text-muted">(可选)</small></label>
            <textarea v-model="description" class="form-control" rows="3" maxlength="500" placeholder="描述这个图集"></textarea>
          </div>

          <div class="row mb-3">
            <div class="col-md-6">
              <label class="form-label">分类</label>
              <select v-model="category" class="form-select">
                <option value="">未分类</option>
                <option value="科技">科技</option>
                <option value="生活">生活</option>
                <option value="游戏">游戏</option>
                <option value="教育">教育</option>
                <option value="文化">文化</option>
                <option value="其他">其他</option>
              </select>
            </div>
            <div class="col-md-6">
              <label class="form-label">标签</label>
              <input v-model="tagsStr" type="text" class="form-control" placeholder="用逗号分隔">
            </div>
          </div>

          <div class="mb-3">
            <label class="form-label">上传图片</label>
            <div class="upload-zone" @click="triggerUpload" @dragover.prevent @drop.prevent="handleDrop">
              <i class="fa fa-cloud-upload fa-3x text-muted mb-2"></i>
              <p class="mb-1">点击或拖拽图片到此处</p>
              <p class="small text-muted">支持 JPG/PNG/GIF/WebP，每张最大 10MB</p>
              <input ref="fileInput" type="file" multiple accept="image/jpeg,image/png,image/gif,image/webp" hidden @change="handleFiles">
            </div>
            <div v-if="uploading" class="text-center py-3">
              <i class="fa fa-spinner fa-spin me-2"></i>上传中...
            </div>
            <div v-if="previews.length" class="preview-grid">
              <div v-for="(p, i) in previews" :key="i" class="preview-item">
                <img :src="p.url">
                <button type="button" class="remove-btn" @click="removeImage(i)">&times;</button>
              </div>
            </div>
          </div>

          <div class="d-flex gap-2 justify-content-end">
            <NuxtLink to="/galleries" class="btn btn-secondary">取消</NuxtLink>
            <button type="submit" class="btn btn-primary px-4" :disabled="!previews.length || uploading || isBanned">
              发布图集
            </button>
          </div>
        </form>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
const title = ref('')
const description = ref('')
const category = ref('')
const tagsStr = ref('')
const fileInput = ref(null)
const previews = ref<{ file: File; url: string }[]>([])
const uploading = ref(false)

function triggerUpload() { fileInput.value?.click() }

function handleDrop(e) {
  const files = Array.from(e.dataTransfer.files || [])
  addFiles(files)
}

function handleFiles(e) {
  const files = Array.from(e.target.files || [])
  addFiles(files)
  e.target.value = ''
}

function addFiles(files: File[]) {
  for (const file of files) {
    const allowed = ['image/jpeg', 'image/png', 'image/gif', 'image/webp']
    if (!allowed.includes(file.type)) continue
    if (file.size > 10 * 1024 * 1024) continue
    if (previews.value.some(p => p.file.name === file.name && p.file.size === file.size)) continue
    previews.value.push({ file, url: URL.createObjectURL(file) })
  }
}

function removeImage(i: number) {
  URL.revokeObjectURL(previews.value[i].url)
  previews.value.splice(i, 1)
}

async function handleCreate() {
  if (!title.value.trim() || !previews.value.length) return
  uploading.value = true
  try {
    const fd = new FormData()
    for (const p of previews.value) {
      fd.append('files', p.file)
    }
    const uploadRes = await $fetch('/api/upload/gallery-images', { method: 'POST', body: fd })
    if (!uploadRes.success) { alert('图片上传失败'); return }
    await $fetch('/api/articles', {
      method: 'POST',
      body: {
        title: title.value.trim(),
        content: description.value,
        type: 'gallery',
        category: category.value,
        tags: tagsStr.value ? tagsStr.value.split(',').map(t => t.trim()).filter(Boolean) : [],
        images: uploadRes.urls,
      },
    })
    navigateTo('/galleries')
  } catch {
    alert('创建失败')
  } finally {
    uploading.value = false
  }
}
const { banned: isBanned } = useBan()
</script>

<style scoped>
.gallery-creator { max-width: 800px; margin: 0 auto; }
.upload-zone {
  border: 2px dashed #ddd; border-radius: 12px; padding: 40px;
  text-align: center; cursor: pointer; transition: border-color 0.3s;
}
.upload-zone:hover { border-color: #00a1d6; background: #f8f9ff; }
.preview-grid {
  display: grid; grid-template-columns: repeat(auto-fill, minmax(140px, 1fr));
  gap: 12px; margin-top: 12px;
}
.preview-item {
  position: relative; border-radius: 8px; overflow: hidden; aspect-ratio: 1;
}
.preview-item img { width: 100%; height: 100%; object-fit: cover; }
.remove-btn {
  position: absolute; top: 4px; right: 4px; width: 24px; height: 24px;
  border-radius: 50%; border: none; background: rgba(0,0,0,0.5); color: #fff;
  font-size: 16px; cursor: pointer; display: flex; align-items: center; justify-content: center;
  line-height: 1; transition: background 0.2s;
}
.remove-btn:hover { background: rgba(255,0,0,0.7); }
</style>
