<template>
  <div class="favorites-page container py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
      <h2 class="mb-0"><i class="fa fa-heart me-2"></i>我的收藏</h2>
      <button class="btn btn-outline-primary btn-sm" @click="showNewFolder = true"><i class="fa fa-plus me-1"></i>新建收藏夹</button>
    </div>

    <div class="row mb-4">
      <div class="col-12">
        <ul class="nav nav-pills">
          <li class="nav-item">
            <a class="nav-link" :class="{ active: !activeFolderId }" href="#" @click.prevent="activeFolderId = null; fetchFavorites()">全部</a>
          </li>
          <li v-for="f in folders" :key="f.id" class="nav-item">
            <a class="nav-link" :class="{ active: activeFolderId === f.id }" href="#" @click.prevent="activeFolderId = f.id; fetchFavorites()">
              {{ f.name }} <span class="badge bg-secondary ms-1">{{ f.video_count }}</span>
            </a>
          </li>
        </ul>
      </div>
    </div>

    <div class="row g-3">
      <div v-for="fav in favorites" :key="fav.id" class="col-md-4 col-sm-6">
        <div class="card h-100 fav-card">
          <div class="position-relative">
            <NuxtLink :to="'/player/' + fav.slug">
              <img :src="fav.image_url || '/images/videoImg.webp'" alt="" class="card-img-top" height="150" style="object-fit:cover;">
            </NuxtLink>
            <button class="btn btn-sm btn-danger position-absolute top-0 end-0 m-1" @click="removeFavorite(fav.id)" title="取消收藏">
              <i class="fa fa-times"></i>
            </button>
          </div>
          <div class="card-body">
            <h6 class="card-title text-truncate"><NuxtLink :to="'/player/' + fav.slug" class="text-decoration-none">{{ fav.title }}</NuxtLink></h6>
            <small class="text-muted">{{ fav.author }} · {{ fav.watch_volue || 0 }}播放</small>
          </div>
        </div>
      </div>
    </div>

    <div v-if="!favorites.length" class="empty-state text-center py-5">
      <i class="fa fa-folder-open fa-3x text-muted mb-3"></i>
      <p class="text-muted">收藏夹为空</p>
      <NuxtLink to="/" class="btn btn-outline-primary">去发现内容</NuxtLink>
    </div>

    <div v-if="showNewFolder" class="modal-backdrop" @click.self="showNewFolder = false">
      <div class="modal-content" @click.stop>
        <div class="modal-header"><h5>新建收藏夹</h5><button class="btn-close" @click="showNewFolder = false"></button></div>
        <div class="modal-body">
          <div class="mb-3">
            <label class="form-label">名称</label>
            <input v-model="newFolderName" class="form-control" maxlength="100" placeholder="收藏夹名称">
          </div>
          <div class="mb-3">
            <label class="form-label">描述</label>
            <textarea v-model="newFolderDesc" class="form-control" rows="2"></textarea>
          </div>
          <div class="form-check mb-3">
            <input v-model="newFolderPublic" type="checkbox" class="form-check-input" id="pub">
            <label class="form-check-label" for="pub">公开收藏夹</label>
          </div>
        </div>
        <div class="modal-footer">
          <button class="btn btn-primary" @click="createFolder">创建</button>
          <button class="btn btn-secondary" @click="showNewFolder = false">取消</button>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
const folders = ref([])
const favorites = ref([])
const activeFolderId = ref(null)
const showNewFolder = ref(false)
const newFolderName = ref('')
const newFolderDesc = ref('')
const newFolderPublic = ref(true)

async function fetchFavorites() {
  try {
    const params = {}
    if (activeFolderId.value) params.folder_id = activeFolderId.value
    const res = await $fetch('/api/favorites', { params })
    if (res.success) {
      folders.value = res.data.folders
      favorites.value = res.data.favorites
    }
  } catch {
  }
}

async function removeFavorite(favId) {
  try {
    await $fetch('/api/favorites', { method: 'POST', body: { video_id: favId } })
    favorites.value = favorites.value.filter(f => f.id !== favId)
  } catch {
    alert('操作失败')
  }
}

async function createFolder() {
  if (!newFolderName.value) return
  try {
    const res = await $fetch('/api/favorites/folders', {
      method: 'POST',
      body: { name: newFolderName.value, description: newFolderDesc.value, is_public: newFolderPublic.value }
    })
    if (res.success) {
      showNewFolder.value = false
      newFolderName.value = ''
      newFolderDesc.value = ''
      fetchFavorites()
    }
  } catch {
    alert('创建失败')
  }
}

fetchFavorites()
</script>

<style scoped>
.favorites-page { max-width: 1000px; margin: 0 auto; }
.fav-card { transition: transform 0.2s; }
.fav-card:hover { transform: translateY(-3px); }
.modal-backdrop { position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.5); z-index: 1050; display: flex; align-items: center; justify-content: center; }
.modal-content { background: #fff; border-radius: 8px; width: 90%; max-width: 500px; }
.modal-header, .modal-footer { padding: 1rem; border-bottom: 1px solid #eee; }
.modal-footer { border-top: 1px solid #eee; border-bottom: none; }
.modal-body { padding: 1rem; }
</style>
