<template>
  <div class="home-page">
    <section class="hero-section">
      <div class="hero-carousel">
        <Carousel :items="carouselItems" />
      </div>
    </section>

    <section class="category-tabs">
      <button v-for="cat in categories" :key="cat.key"
        :class="{ active: activeCategory === cat.key }"
        @click="activeCategory = cat.key">
        {{ cat.label }}
      </button>
    </section>

    <section class="video-feed">
      <div class="video-grid">
        <article v-for="v in filteredVideos" :key="v.id" class="video-card">
          <NuxtLink :to="v.link" class="card-link">
            <div class="card-thumb">
              <img :src="v.img || '/images/videoImg.webp'" :alt="v.title" loading="lazy">
              <span class="card-duration">{{ v.videoTime || '00:00' }}</span>
            </div>
            <div class="card-info">
              <h3 class="card-title">{{ v.title }}</h3>
              <div class="card-meta">
                <span class="card-author">
                  <img v-if="v.authorImg" :src="v.authorImg" alt="" class="card-avatar">
                  <i v-else class="fa fa-user-circle-o"></i>
                  {{ v.author }}
                </span>
                <span class="card-stats"><i class="fa fa-eye"></i> {{ v.watchVolue }}</span>
              </div>
            </div>
          </NuxtLink>
        </article>
      </div>
      <div v-if="!allVideos.length" class="empty-feed">
        <i class="fa fa-film fa-3x"></i>
        <p>还没有视频，快来投稿吧~</p>
        <NuxtLink to="/upload" class="btn btn-primary">投稿</NuxtLink>
      </div>
    </section>
  </div>
</template>

<script setup>
const activeCategory = ref('all')

const categories = [
  { key: 'all', label: '推荐' },
  { key: '动画', label: '动画' },
  { key: '音乐', label: '音乐' },
  { key: '游戏', label: '游戏' },
  { key: '知识', label: '知识' },
  { key: '科技', label: '科技' },
  { key: '生活', label: '生活' },
  { key: '美食', label: '美食' },
  { key: '娱乐', label: '娱乐' },
]

const { data: homeData } = await useFetch('/api/home', { default: () => ({}) })

const carouselItems = computed(() => {
  const list = homeData.value?.carouselList || []
  return list.map(item => ({
    link: item.link || item.image_url || '',
    url: item.url || item.link || '/',
    title: item.title || ''
  }))
})

const allVideos = computed(() => {
  const obj = homeData.value?.videoObjectList
  if (!obj) return []
  const items = []
  for (const section of [obj.specialObject, obj.defaultObject, obj.imageObject]) {
    if (section?.videosLineList) {
      for (const chunk of section.videosLineList) {
        items.push(...chunk)
      }
    }
  }
  return items
})

const filteredVideos = computed(() => {
  if (activeCategory.value === 'all') return allVideos.value
  return allVideos.value.filter(v => v.videoType === activeCategory.value)
})
</script>

<style scoped>
.home-page {
  max-width: 1200px; margin: 0 auto; padding: 20px 16px 40px;
}
.hero-section {
  margin-bottom: 24px; border-radius: 12px; overflow: hidden;
  box-shadow: 0 2px 12px rgba(0,0,0,0.08);
}
.category-tabs {
  display: flex; gap: 4px; margin-bottom: 20px; flex-wrap: wrap;
}
.category-tabs button {
  background: none; border: none; padding: 6px 16px; border-radius: 20px;
  font-size: 14px; color: #666; cursor: pointer; transition: all 0.2s;
  font-weight: 500;
}
.category-tabs button:hover { background: #f0f0f0; color: #333; }
.category-tabs button.active { background: #00a1d6; color: #fff; }
.video-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
  gap: 20px;
}
.video-card {
  border-radius: 10px; overflow: hidden; transition: transform 0.2s;
  background: #fff; box-shadow: 0 1px 4px rgba(0,0,0,0.06);
}
.video-card:hover { transform: translateY(-4px); box-shadow: 0 4px 20px rgba(0,0,0,0.12); }
.card-link { text-decoration: none; color: inherit; display: block; }
.card-thumb { position: relative; width: 100%; aspect-ratio: 16 / 9; overflow: hidden; background: #f0f0f0; }
.card-thumb img { width: 100%; height: 100%; object-fit: cover; transition: transform 0.3s; }
.video-card:hover .card-thumb img { transform: scale(1.05); }
.card-duration {
  position: absolute; bottom: 6px; right: 6px;
  background: rgba(0,0,0,0.7); color: #fff; font-size: 12px;
  padding: 1px 6px; border-radius: 4px;
}
.card-info { padding: 10px 12px 14px; }
.card-title {
  font-size: 14px; font-weight: 600; margin: 0 0 6px;
  overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: #222;
}
.card-meta { display: flex; justify-content: space-between; font-size: 12px; color: #999; }
.card-author i { margin-right: 3px; }
.card-avatar { width: 18px; height: 18px; border-radius: 50%; object-fit: cover; vertical-align: middle; margin-right: 4px; }
.card-stats i { margin-right: 3px; }
.empty-feed { text-align: center; padding: 80px 20px; color: #999; }
.empty-feed i { margin-bottom: 16px; }
.empty-feed p { margin-bottom: 16px; }
</style>
