<template>
  <div class="user-center container py-4">
    <div class="row">
      <div class="col-md-3">
        <div class="user-sidebar card">
          <div class="card-body text-center">
            <div class="user-avatar mb-3 position-relative">
              <img :src="profile.avatar_url || '/images/authorImg.webp'" alt="头像" class="rounded-circle" width="80" height="80">
              <button class="btn btn-sm btn-outline-secondary mt-2" @click="showAvatarUpload = true">更换头像</button>
            </div>
            <h5 class="user-name">{{ profile.display_name || profile.username }}</h5>
            <p class="text-muted small">UID: {{ profile.id || '---' }}</p>
            <p class="small text-muted">{{ profile.bio }}</p>
            <div class="user-stats d-flex justify-content-around mt-3">
              <div class="stat-item">
                <div class="stat-number">{{ profile.following || 0 }}</div>
                <div class="stat-label">关注</div>
              </div>
              <div class="stat-item">
                <div class="stat-number">{{ profile.followers || 0 }}</div>
                <div class="stat-label">粉丝</div>
              </div>
              <div class="stat-item">
                <div class="stat-number">{{ profile.likes || 0 }}</div>
                <div class="stat-label">获赞</div>
              </div>
            </div>
          </div>
          <ul class="list-group list-group-flush">
            <li class="list-group-item">
              <NuxtLink to="/user/center" class="text-decoration-none"><i class="fa fa-user me-2"></i>个人主页</NuxtLink>
            </li>
            <li class="list-group-item">
              <a href="#" class="text-decoration-none" @click.prevent="showEdit = true"><i class="fa fa-cog me-2"></i>编辑资料</a>
            </li>
            <li class="list-group-item">
              <NuxtLink to="/favorites" class="text-decoration-none"><i class="fa fa-heart me-2"></i>我的收藏 <span class="badge bg-secondary">{{ stats.favorites }}</span></NuxtLink>
            </li>
            <li class="list-group-item">
              <NuxtLink to="/history" class="text-decoration-none"><i class="fa fa-history me-2"></i>观看历史 <span class="badge bg-secondary">{{ stats.history }}</span></NuxtLink>
            </li>
          </ul>
        </div>
      </div>
      <div class="col-md-9">
        <div class="card mb-4">
          <div class="card-header d-flex justify-content-between align-items-center">
            <span><i class="fa fa-video-camera me-2"></i>我的投稿</span>
            <NuxtLink to="/upload" class="btn btn-primary btn-sm"><i class="fa fa-upload me-1"></i>投稿</NuxtLink>
          </div>
          <div class="card-body">
            <div v-if="myVideos.length" class="row g-3">
              <div v-for="v in myVideos" :key="v.id" class="col-md-6">
                <div class="d-flex gap-3 align-items-start">
                  <img :src="v.image_url || '/images/videoImg.webp'" alt="" width="120" height="68" style="object-fit:cover;border-radius:4px;">
                  <div class="flex-grow-1">
                    <h6 class="mb-1"><NuxtLink :to="'/player/' + v.slug">{{ v.title }}</NuxtLink></h6>
                    <small class="text-muted">{{ v.video_type }} · {{ v.watch_volue || 0 }}播放</small>
                  </div>
                  <NuxtLink :to="'/edit/' + v.id" class="btn btn-outline-primary btn-sm mt-1">
                    <i class="fa fa-pencil"></i>
                  </NuxtLink>
                </div>
              </div>
            </div>
            <div v-else class="empty-state text-center py-5">
              <i class="fa fa-video-camera fa-3x text-muted mb-3"></i>
              <p class="text-muted">还没有投稿内容</p>
              <NuxtLink to="/upload" class="btn btn-primary">立即投稿</NuxtLink>
            </div>
          </div>
        </div>

        <div class="card">
          <div class="card-header"><i class="fa fa-rss me-2"></i>我的动态</div>
          <div class="card-body">
            <div v-if="activities.length">
              <div v-for="act in activities" :key="act.id" class="d-flex gap-2 mb-3 pb-2 border-bottom">
                <img :src="act.avatar_url || '/images/authorImg.webp'" alt="" width="36" height="36" class="rounded-circle">
                <div>
                  <strong>{{ act.display_name || act.username }}</strong>
                  <span class="text-muted small ms-2">{{ act.type }}</span>
                  <p class="mb-0 small">{{ act.content }}</p>
                  <small class="text-muted">{{ new Date(act.created_at).toLocaleDateString() }}</small>
                </div>
              </div>
            </div>
            <div v-else class="empty-state text-center py-4">
              <p class="text-muted">暂无动态</p>
            </div>
          </div>
        </div>
      </div>
    </div>

    <div v-if="showEdit" class="modal-backdrop" @click.self="showEdit = false">
      <div class="modal-content" @click.stop>
        <div class="modal-header"><h5>编辑资料</h5><button class="btn-close" @click="showEdit = false"></button></div>
        <div class="modal-body">
          <div class="mb-3">
            <label class="form-label">显示名称 (昵称)</label>
            <input v-model="editForm.display_name" class="form-control" maxlength="30" placeholder="可选，用于公开显示">
          </div>
          <div class="mb-3">
            <label class="form-label">个人简介</label>
            <textarea v-model="editForm.bio" class="form-control" rows="3" maxlength="200"></textarea>
          </div>
          <div class="mb-3">
            <label class="form-label">性别</label>
            <select v-model="editForm.gender" class="form-select">
              <option value="">保密</option>
              <option value="男">男</option>
              <option value="女">女</option>
            </select>
          </div>
          <div class="mb-3">
            <label class="form-label">所在地</label>
            <input v-model="editForm.location" class="form-control" maxlength="100">
          </div>
          <div class="mb-3">
            <label class="form-label">个人网站</label>
            <input v-model="editForm.website" class="form-control" maxlength="500">
          </div>
        </div>
        <div class="modal-footer">
          <button class="btn btn-primary" @click="saveProfile">保存</button>
          <button class="btn btn-secondary" @click="showEdit = false">取消</button>
        </div>
      </div>
    </div>

    <div v-if="showAvatarUpload" class="modal-backdrop" @click.self="closeAvatarModal">
      <div class="modal-content" @click.stop>
        <div class="modal-header"><h5>更换头像</h5><button class="btn-close" @click="closeAvatarModal"></button></div>
        <div class="modal-body text-center">
          <div v-if="!avatarFile">
            <img :src="profile.avatar_url || '/images/authorImg.webp'" alt="" class="rounded-circle mb-3" width="120" height="120" style="object-fit:cover;">
            <input type="file" accept="image/*" class="form-control mb-3" @change="handleAvatarFile">
          </div>

          <div v-else-if="avatarFile && !needsCrop">
            <img :src="previewUrl" alt="" class="mb-3" style="max-width:100%;border-radius:4px;">
            <p class="text-success small">标准头像比例，可直接保存</p>
            <div class="d-flex gap-2 justify-content-center">
              <button class="btn btn-primary" @click="uploadAvatar" :disabled="uploading">{{ uploading ? '上传中...' : '保存' }}</button>
              <button class="btn btn-secondary" @click="closeAvatarModal">取消</button>
            </div>
          </div>

          <div v-else>
            <div ref="cropperContainer" style="width:100%;height:50vh;min-height:280px;max-height:500px;margin:0 auto;display:flex;">
              <img ref="cropImageEl" :src="previewUrl" style="max-width:100%;max-height:100%;">
            </div>
            <p class="text-warning small mt-2">图片非正方形，请裁剪为正方形头像</p>
            <div class="d-flex gap-2 justify-content-center mt-2">
              <button class="btn btn-primary" @click="cropAndUpload" :disabled="uploading">{{ uploading ? '上传中...' : '裁剪并保存' }}</button>
              <button class="btn btn-secondary" @click="closeAvatarModal">取消</button>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onBeforeUnmount } from 'vue'

const profile = ref({ username: '用户', id: '' })
const myVideos = ref([])
const activities = ref([])
const stats = ref({ favorites: 0, history: 0 })
const showEdit = ref(false)
const showAvatarUpload = ref(false)
const editForm = ref({ display_name: '', bio: '', gender: '', location: '', website: '' })
const avatarFile = ref(null)
const previewUrl = ref('')
const uploading = ref(false)
const needsCrop = ref(false)
const cropperContainer = ref(null)
const cropImageEl = ref(null)
let cropper = null

async function fetchData() {
  try {
    const [profRes, statsRes, actRes] = await Promise.all([
      $fetch('/api/user/profile'),
      $fetch('/api/user/stats'),
      $fetch('/api/dynamics'),
    ])
    if (profRes.success) {
      profile.value = profRes.data
      editForm.value = { display_name: profRes.data.display_name || '', bio: profRes.data.bio || '', gender: profRes.data.gender || '', location: profRes.data.location || '', website: profRes.data.website || '' }
    }
    if (statsRes.success) stats.value = statsRes.data
    if (actRes.success) activities.value = actRes.data
  } catch (e) {
    if (e?.statusCode === 401) navigateTo('/login')
  }
  try {
    const vidRes = await $fetch('/api/videos', { params: { author: profile.value.username } })
    myVideos.value = vidRes.rows || []
  } catch {
  }
}

async function saveProfile() {
  try {
    const res = await $fetch('/api/user/profile', { method: 'PUT', body: editForm.value })
    if (res.success) {
      showEdit.value = false
      profile.value = { ...profile.value, ...editForm.value }
      const { fetchUser } = useUser()
      await fetchUser()
    }
  } catch {
    alert('保存失败')
  }
}

async function saveDisplayName() {
  try {
    const res = await $fetch('/api/user/display-name', { method: 'POST', body: { display_name: editForm.value.display_name } })
    if (res.success) {
      profile.value.display_name = res.display_name
      // 刷新页面以更新导航栏显示
      window.location.reload()
    }
  } catch {
    alert('更新失败')
  }
}

let CropperClass = null

async function loadCropper() {
  if (CropperClass) return CropperClass
  const mod = await import('cropperjs')
  CropperClass = mod.default || mod
  return CropperClass
}

function handleAvatarFile(e) {
  const file = e.target.files?.[0]
  if (!file) return

  const url = URL.createObjectURL(file)
  previewUrl.value = url
  avatarFile.value = file

  const img = new Image()
  img.onload = () => {
    const ratio = img.naturalWidth / img.naturalHeight
    if (ratio >= 0.95 && ratio <= 1.05) {
      needsCrop.value = false
    } else {
      needsCrop.value = true
      nextTick(async () => {
        if (cropper) { cropper.destroy(); cropper = null }
        if (cropImageEl.value) {
          const Cropper = await loadCropper()
          cropper = new Cropper(cropImageEl.value)
        }
      })
    }
  }
  img.onerror = () => { needsCrop.value = false }
  img.src = url
}

async function uploadAvatar() {
  if (!avatarFile.value || uploading.value) return
  uploading.value = true
  try {
    const formData = new FormData()
    formData.append('avatar', avatarFile.value)
    const res = await $fetch('/api/user/avatar', { method: 'POST', body: formData })
    if (res.success) {
      profile.value.avatar_url = res.avatar_url
      closeAvatarModal()
    }
  } catch {
    alert('上传失败')
  } finally {
    uploading.value = false
  }
}

async function cropAndUpload() {
  if (!cropper) {
    uploadAvatar()
    return
  }
  try {
    const selection = cropper.getCropperSelection()
    if (!selection) { alert('裁剪组件未就绪'); return }
    const canvas = await selection.$toCanvas({ width: 256, height: 256 })
    canvas.toBlob((blob) => {
      if (!blob) { alert('裁剪失败'); return }
      avatarFile.value = new File([blob], avatarFile.value.name, { type: 'image/png' })
      if (previewUrl.value) URL.revokeObjectURL(previewUrl.value)
      previewUrl.value = URL.createObjectURL(blob)
      if (cropper) { cropper.destroy(); cropper = null }
      needsCrop.value = false
      uploadAvatar()
    }, 'image/png')
  } catch {
    alert('裁剪失败')
  }
}

function closeAvatarModal() {
  if (cropper) { cropper.destroy(); cropper = null }
  if (previewUrl.value) URL.revokeObjectURL(previewUrl.value)
  showAvatarUpload.value = false
  avatarFile.value = null
  previewUrl.value = ''
  needsCrop.value = false
}

onBeforeUnmount(() => {
  if (cropper) { cropper.destroy(); cropper = null }
  if (previewUrl.value) { URL.revokeObjectURL(previewUrl.value) }
})

fetchData()
</script>

<style scoped>
.user-sidebar { margin-bottom: 20px; }
.user-avatar img { object-fit: cover; border: 3px solid #f0f0f0; }
.user-stats { border-top: 1px solid #eee; padding-top: 15px; }
.stat-item { text-align: center; }
.stat-number { font-size: 18px; font-weight: 700; color: #333; }
.stat-label { font-size: 12px; color: #999; }
.modal-backdrop { position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.5); z-index: 1050; display: flex; align-items: center; justify-content: center; }
.modal-content { background: #fff; border-radius: 8px; width: 90%; max-width: 500px; max-height: 80vh; overflow-y: auto; }
.modal-header { padding: 1rem; border-bottom: 1px solid #eee; display: flex; justify-content: space-between; align-items: center; }
.modal-body { padding: 1rem; }
.modal-footer { padding: 1rem; border-top: 1px solid #eee; display: flex; gap: 0.5rem; justify-content: flex-end; }
</style>
