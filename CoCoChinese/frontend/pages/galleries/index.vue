<template>
  <div class="gallery-list-page container py-4">
    <div class="d-flex justify-content-between align-items-center mb-4">
      <h2 class="mb-0"><i class="fa fa-file-image-o me-2"></i>图集</h2>
      <NuxtLink to="/create/gallery" class="btn btn-primary"><i class="fa fa-plus me-1"></i>创建图集</NuxtLink>
    </div>

    <div v-if="loading" class="text-center py-5">
      <i class="fa fa-spinner fa-spin fa-2x text-muted"></i>
    </div>

    <template v-else>
      <div v-if="!items.length" class="empty-state text-center py-5">
        <i class="fa fa-images fa-3x text-muted mb-3"></i>
        <p class="text-muted">还没有图集</p>
        <NuxtLink to="/create/gallery" class="btn btn-primary">创建第一个图集</NuxtLink>
      </div>

      <div class="gallery-grid">
        <div v-for="item in items" :key="item.id" class="gallery-card card">
          <NuxtLink :to="'/galleries/' + item.slug" class="text-decoration-none">
            <div class="gallery-cover">
              <img :src="coverImage(item)" :alt="item.title">
              <span class="image-count"><i class="fa fa-image me-1"></i>{{ item.images?.length || 0 }}</span>
            </div>
            <div class="card-body">
              <h5 class="gallery-title">{{ item.title }}</h5>
              <p class="gallery-meta text-muted small">
                {{ item.author }} · {{ new Date(item.created_at).toLocaleDateString() }}
              </p>
            </div>
          </NuxtLink>
        </div>
      </div>
    </template>
  </div>
</template>

<script setup>
const items = ref([])
const loading = ref(true)

function coverImage(item) {
  if (item.cover_image) return item.cover_image
  if (item.images?.length) return item.images[0]
  return '/images/videoImg.webp'
}

async function fetchGalleries() {
  try {
    const res = await $fetch('/api/articles', { params: { type: 'gallery' } })
    if (res.success) items.value = res.data
  } catch {} finally {
    loading.value = false
  }
}

fetchGalleries()
</script>

<style scoped>
.gallery-list-page { max-width: 1000px; margin: 0 auto; }
.gallery-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 20px; }
.gallery-card { transition: transform 0.2s, box-shadow 0.2s; overflow: hidden; }
.gallery-card:hover { transform: translateY(-2px); box-shadow: 0 4px 16px rgba(0,0,0,0.1); }
.gallery-cover { position: relative; width: 100%; height: 200px; overflow: hidden; }
.gallery-cover img { width: 100%; height: 100%; object-fit: cover; transition: transform 0.3s; }
.gallery-card:hover .gallery-cover img { transform: scale(1.05); }
.image-count {
  position: absolute; bottom: 8px; right: 8px; background: rgba(0,0,0,0.6);
  color: #fff; padding: 2px 10px; border-radius: 12px; font-size: 12px;
}
.gallery-title { font-size: 16px; font-weight: 600; color: #222; margin-bottom: 4px; }
.gallery-meta { font-size: 12px; }
.empty-state p { margin: 0; }
</style>
