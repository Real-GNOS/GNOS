<template>
  <div class="playlist-detail-page">
    <div v-if="playlist" class="playlist-detail">
      <div class="playlist-header">
        <div>
          <h1>{{ playlist.name }}</h1>
          <p v-if="playlist.description" class="playlist-desc">{{ playlist.description }}</p>
          <span class="playlist-count"><i class="fa fa-film"></i> {{ videos.length }} 个视频</span>
        </div>
      </div>

      <div v-if="videos.length" class="video-list">
        <div v-for="(v, i) in videos" :key="v.id" class="video-row fluent-card">
          <span class="video-index">{{ i + 1 }}</span>
          <NuxtLink :to="`/player/${v.slug}`" class="video-link">
            <div class="video-thumb">
              <img :src="v.image_url || '/images/videoImg.webp'" :alt="v.title" />
              <span class="video-duration">{{ v.video_time }}</span>
            </div>
            <div class="video-info">
              <h3>{{ v.title }}</h3>
              <span class="video-author">{{ v.author }}</span>
            </div>
          </NuxtLink>
        </div>
      </div>
      <div v-else class="empty-state">
        <i class="fa fa-film fa-4x"></i>
        <p>播放列表中还没有视频</p>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
const route = useRoute()
const playlist = ref<any>(null)
const videos = ref<any[]>([])

onMounted(async () => {
  const id = route.params.id
  try {
    const res: any = await $fetch(`/api/playlists/${id}/videos`)
    playlist.value = res.playlist
    videos.value = res.videos
  } catch {}
})
</script>

<style scoped>
.playlist-detail-page { max-width: 900px; margin: 0 auto; padding: var(--fluent-spacing-2xl) var(--fluent-spacing-xl); }
.playlist-header { margin-bottom: var(--fluent-spacing-2xl); }
.playlist-header h1 { font-size: var(--fluent-font-size-title-large); margin: 0 0 var(--fluent-spacing-xs); }
.playlist-desc { color: var(--fluent-text-secondary); margin: 0 0 var(--fluent-spacing-sm); }
.playlist-count { color: var(--fluent-text-secondary); font-size: var(--fluent-font-size-body); }
.playlist-count i { margin-right: 4px; }
.video-list { display: flex; flex-direction: column; gap: var(--fluent-spacing-sm); }
.video-row { display: flex; align-items: center; gap: var(--fluent-spacing-md); padding: var(--fluent-spacing-md); }
.video-index { font-weight: 700; color: var(--fluent-text-secondary); min-width: 24px; text-align: center; }
.video-link { display: flex; gap: var(--fluent-spacing-md); flex: 1; text-decoration: none; color: inherit; }
.video-thumb { position: relative; width: 160px; flex-shrink: 0; border-radius: var(--fluent-radius-md); overflow: hidden; }
.video-thumb img { width: 100%; aspect-ratio: 16/9; object-fit: cover; }
.video-duration { position: absolute; bottom: 4px; right: 4px; background: rgba(0,0,0,0.7); color: white; font-size: 11px; padding: 1px 5px; border-radius: 3px; }
.video-info { flex: 1; display: flex; flex-direction: column; justify-content: center; }
.video-info h3 { font-size: var(--fluent-font-size-body); margin: 0 0 4px; font-weight: 600; }
.video-author { font-size: var(--fluent-font-size-caption); color: var(--fluent-text-secondary); }
.empty-state { text-align: center; padding: var(--fluent-spacing-4xl) 0; color: var(--fluent-text-secondary); }
</style>
