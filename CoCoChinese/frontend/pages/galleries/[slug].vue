<template>
  <div class="gallery-page container py-4">
    <div v-if="loading" class="text-center py-5">
      <i class="fa fa-spinner fa-spin fa-2x text-muted"></i>
    </div>

    <div v-else-if="!gallery" class="text-center py-5">
      <p class="text-muted">图集不存在</p>
      <NuxtLink to="/galleries" class="btn btn-primary">返回图集列表</NuxtLink>
    </div>

    <template v-else>
      <div class="gallery-header">
        <h1 class="gallery-title">{{ gallery.title }}</h1>
        <div class="gallery-meta">
          <span class="meta-item"><i class="fa fa-user me-1"></i>{{ gallery.author }}</span>
          <span class="meta-item"><i class="fa fa-image me-1"></i>{{ gallery.images?.length || 0 }} 张图片</span>
          <span class="meta-item"><i class="fa fa-clock-o me-1"></i>{{ new Date(gallery.created_at).toLocaleDateString() }}</span>
        </div>
        <div v-if="gallery.tags?.length" class="gallery-tags">
          <span v-for="tag in gallery.tags" :key="tag" class="badge bg-light text-dark me-1">{{ tag }}</span>
        </div>
        <div v-if="gallery.content" class="gallery-desc">{{ gallery.content }}</div>
        <div v-if="isOwner" class="mt-3">
          <button class="btn btn-outline-danger btn-sm" @click="handleDelete"><i class="fa fa-trash me-1"></i>删除</button>
        </div>
      </div>

      <div class="gallery-images">
        <div v-for="(img, i) in gallery.images" :key="i" class="gallery-image-wrap" @click="openViewer(i)">
          <img :src="img" :alt="'图片 ' + (i + 1)">
        </div>
      </div>

      <div v-if="showViewer" class="image-viewer-overlay" @click.self="closeViewer">
        <button class="viewer-close" @click="closeViewer">&times;</button>
        <button v-if="viewerIndex > 0" class="viewer-prev" @click="viewerIndex--">&lsaquo;</button>
        <img :src="gallery.images[viewerIndex]" @click="closeViewer">
        <button v-if="viewerIndex < gallery.images.length - 1" class="viewer-next" @click="viewerIndex++">&rsaquo;</button>
        <div class="viewer-counter">{{ viewerIndex + 1 }} / {{ gallery.images.length }}</div>
      </div>
    </template>
  </div>
</template>

<script setup>
const route = useRoute()
const gallery = ref(null)
const loading = ref(true)
const user = ref(null)
const showViewer = ref(false)
const viewerIndex = ref(0)

const isOwner = computed(() => user.value && gallery.value && user.value.username === gallery.value.author)

function openViewer(i) {
  viewerIndex.value = i
  showViewer.value = true
}

function closeViewer() {
  showViewer.value = false
}

async function fetchGallery() {
  try {
    const res = await $fetch(`/api/articles/${route.params.slug}`)
    if (res.success) gallery.value = res.data
  } catch {} finally {
    loading.value = false
  }
}

async function handleDelete() {
  if (!confirm('确定删除这个图集？')) return
  try {
    await $fetch(`/api/articles/${route.params.slug}`, { method: 'DELETE' })
    navigateTo('/galleries')
  } catch {
    alert('删除失败')
  }
}

onMounted(async () => {
  try {
    const me = await $fetch('/api/user/me', { default: () => null, transform: r => r.user || null })
    if (me) user.value = me
  } catch {}
  await fetchGallery()
})
</script>

<style scoped>
.gallery-page { max-width: 960px; margin: 0 auto; }
.gallery-header { margin-bottom: 32px; }
.gallery-title { font-size: 28px; font-weight: 700; color: #222; margin-bottom: 12px; }
.gallery-meta { display: flex; flex-wrap: wrap; gap: 16px; margin-bottom: 12px; }
.meta-item { font-size: 13px; color: #999; }
.gallery-tags { margin-bottom: 8px; }
.gallery-desc { font-size: 15px; color: #666; line-height: 1.6; margin-bottom: 12px; }
.gallery-images {
  display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
  gap: 12px; margin-bottom: 40px;
}
.gallery-image-wrap {
  border-radius: 8px; overflow: hidden; cursor: pointer;
  transition: transform 0.2s; position: relative;
}
.gallery-image-wrap:hover { transform: scale(1.02); }
.gallery-image-wrap img { width: 100%; display: block; border-radius: 8px; }

.image-viewer-overlay {
  position: fixed; top: 0; left: 0; width: 100%; height: 100%;
  background: rgba(0,0,0,0.9); z-index: 2000; display: flex;
  align-items: center; justify-content: center;
}
.image-viewer-overlay img {
  max-width: 90%; max-height: 90%; border-radius: 8px; object-fit: contain;
}
.viewer-close {
  position: fixed; top: 20px; right: 20px; background: none; border: none;
  color: #fff; font-size: 36px; cursor: pointer; z-index: 2001;
}
.viewer-prev, .viewer-next {
  position: fixed; top: 50%; transform: translateY(-50%);
  background: rgba(255,255,255,0.1); border: none; color: #fff;
  font-size: 48px; padding: 8px 16px; cursor: pointer; border-radius: 8px;
  transition: background 0.2s; z-index: 2001;
}
.viewer-prev:hover, .viewer-next:hover { background: rgba(255,255,255,0.2); }
.viewer-prev { left: 20px; }
.viewer-next { right: 20px; }
.viewer-counter {
  position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%);
  color: #fff; font-size: 14px; background: rgba(0,0,0,0.5);
  padding: 4px 16px; border-radius: 20px;
}
</style>
