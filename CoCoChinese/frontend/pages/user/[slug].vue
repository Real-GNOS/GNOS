<template>
  <div class="profile-page">
    <div v-if="data?.myBan" class="self-ban-banner">
      <i class="fa fa-exclamation-circle"></i>
      <span>你是被封禁的{{ data.myBan.expires_at ? '至 ' + formatBanExpire(data.myBan.expires_at) : '（永久）' }}，原因：{{ data.myBan.reason }}。封禁期间你无法发布内容、评论、上传等。</span>
    </div>
    <div v-else-if="data?.banned" class="other-ban-banner">
      <i class="fa fa-exclamation-circle"></i>
      <span>该用户已被封禁</span>
    </div>
    <div class="profile-toolbar">
      <button :class="{ active: activeTab === 'home' }" @click="activeTab = 'home'">主页</button>
      <button :class="{ active: activeTab === 'works' }" @click="activeTab = 'works'">投稿</button>
    </div>

    <div v-if="activeTab === 'home'" class="home-view">
      <div class="profile-header" v-if="data">
        <div class="profile-avatar">
          <img :src="data.profile.avatar_url || '/images/default_avatar.png'" alt="">
        </div>
        <div class="profile-info">
          <h2>{{ data.profile.display_name || data.profile.username }}</h2>
          <p class="text-muted">@{{ data.profile.username }}</p>
          <p v-if="data.profile.bio" class="bio">{{ data.profile.bio }}</p>
          <div class="profile-meta">
            <span><strong>{{ data.profile.following }}</strong> 关注</span>
            <span><strong>{{ data.profile.followers }}</strong> 粉丝</span>
            <span><strong>{{ data.profile.likes }}</strong> 获赞</span>
            <span>加入于 {{ formatDate(data.profile.created_at) }}</span>
          </div>
          <div class="profile-actions" v-if="user && user.id !== data.profile.id">
            <button class="btn btn-follow" :class="{ following: isFollowing }" @click="toggleFollow">
              {{ isFollowing ? '已关注' : '关注' }}
            </button>
            <button class="btn btn-message" @click="sendDM">
              <i class="fa fa-envelope me-1"></i>私信
            </button>
          </div>
        </div>
      </div>

      <div class="profile-videos" v-if="data?.videos.length">
        <h4>投稿视频</h4>
        <div class="video-grid">
          <NuxtLink v-for="v in data.videos" :key="v.id" :to="'/player/' + v.slug" class="video-card">
            <div class="thumb">
              <img :src="v.image_url || '/images/videoImg.webp'" alt="">
              <span class="duration">{{ v.video_time }}</span>
            </div>
            <p class="title">{{ v.title }}</p>
          </NuxtLink>
        </div>
      </div>
      <div v-else class="empty-tip">
        <i class="fa fa-file-video-o fa-3x text-muted mb-2"></i>
        <p class="text-muted">还没有投稿视频</p>
      </div>
    </div>

    <div v-if="activeTab === 'works'" class="works-view">
      <div class="works-sidebar">
        <div
          v-for="item in workTypes"
          :key="item.key"
          :class="{ active: activeWorkType === item.key }"
          @click="activeWorkType = item.key"
        >
          <i :class="'fa ' + item.icon + ' me-2'"></i>{{ item.label }}
        </div>
      </div>
      <div class="works-content">
        <h4>{{ currentTypeLabel }}</h4>
        <div v-if="worksLoading" class="text-center py-4">
          <i class="fa fa-spinner fa-spin fa-2x text-muted"></i>
        </div>
        <template v-else-if="activeWorkType === 'video'">
          <div v-if="data?.videos.length" class="video-grid">
            <NuxtLink v-for="v in data.videos" :key="v.id" :to="'/player/' + v.slug" class="video-card">
              <div class="thumb">
                <img :src="v.image_url || '/images/videoImg.webp'" alt="">
                <span class="duration">{{ v.video_time }}</span>
              </div>
              <p class="title">{{ v.title }}</p>
            </NuxtLink>
          </div>
          <div v-else class="empty-tip">
            <i class="fa fa-file-video-o fa-3x text-muted mb-2"></i>
            <p class="text-muted">还没有视频投稿</p>
          </div>
        </template>
        <template v-else-if="activeWorkType === 'gallery'">
          <div v-if="galleries.length" class="gallery-grid">
            <NuxtLink v-for="g in galleries" :key="g.slug" :to="'/galleries/' + g.slug" class="gallery-card">
              <div class="thumb">
                <img :src="g.images?.[0] || '/images/videoImg.webp'" alt="">
                <span class="image-count"><i class="fa fa-image me-1"></i>{{ g.images?.length || 0 }}</span>
              </div>
              <p class="title">{{ g.title }}</p>
            </NuxtLink>
          </div>
          <div v-else class="empty-tip">
            <i class="fa fa-images fa-3x text-muted mb-2"></i>
            <p class="text-muted">还没有图片投稿</p>
          </div>
        </template>
        <template v-else-if="activeWorkType === 'article'">
          <div v-if="articles.length" class="article-list">
            <NuxtLink v-for="a in articles" :key="a.slug" :to="'/articles/' + a.slug" class="article-card">
              <div v-if="a.cover_image" class="article-cover">
                <img :src="a.cover_image" :alt="a.title">
              </div>
              <div class="article-info">
                <h5>{{ a.title }}</h5>
                <p class="text-muted small">{{ new Date(a.created_at).toLocaleDateString() }}</p>
              </div>
            </NuxtLink>
          </div>
          <div v-else class="empty-tip">
            <i class="fa fa-file-text-o fa-3x text-muted mb-2"></i>
            <p class="text-muted">还没有文章投稿</p>
          </div>
        </template>
      </div>
    </div>
  </div>
</template>

<script setup>
const route = useRoute()
const activeTab = ref('home')
const activeWorkType = ref('video')
const worksLoading = ref(false)
const galleries = ref([])
const articles = ref([])

const workTypes = [
  { key: 'video', label: '视频', icon: 'fa-video-camera' },
  { key: 'gallery', label: '图片', icon: 'fa-image' },
  { key: 'article', label: '文章', icon: 'fa-file-text-o' },
]

const currentTypeLabel = computed(() => workTypes.find(t => t.key === activeWorkType.value)?.label || '')

const { data: userData } = await useFetch('/api/user/me', { default: () => ({ user: null }), transform: r => r.user || null })
const user = computed(() => userData.value)

const { data, refresh } = await useFetch(`/api/user/${route.params.slug}`, {
  default: () => ({ profile: null, videos: [], isFollowing: false })
})

const isFollowing = ref(data.value?.isFollowing || false)

watch(activeWorkType, async (val) => {
  if (val === 'gallery' && !galleries.value.length) await fetchGalleries()
  else if (val === 'article' && !articles.value.length) await fetchArticles()
})

async function fetchGalleries() {
  if (!data.value?.profile?.username) return
  worksLoading.value = true
  try {
    const res = await $fetch('/api/articles', { params: { type: 'gallery', author: data.value.profile.username } })
    if (res.success) galleries.value = res.data
  } catch {} finally {
    worksLoading.value = false
  }
}

async function fetchArticles() {
  if (!data.value?.profile?.username) return
  worksLoading.value = true
  try {
    const res = await $fetch('/api/articles', { params: { type: 'article', author: data.value.profile.username } })
    if (res.success) articles.value = res.data
  } catch {} finally {
    worksLoading.value = false
  }
}

async function toggleFollow() {
  if (!user.value) { navigateTo('/login'); return }
  try {
    const res = await $fetch(`/api/user/${data.value.profile.id}/follow`, { method: 'POST' })
    isFollowing.value = res.following
    refresh()
  } catch (e) { alert(e.data?.message || '操作失败') }
}

function sendDM() {
  if (!user.value) { navigateTo('/login'); return }
  navigateTo(`/messages?dm=${data.value.profile.id}`)
}

function formatDate(d) {
  if (!d) return ''
  return new Date(d).toLocaleDateString('zh-CN')
}

function formatBanExpire(d) {
  if (!d) return ''
  try {
    return new Date(d).toLocaleString('zh-CN')
  } catch {
    return d
  }
}
</script>

<style scoped>
.profile-page { max-width: 1100px; margin: 80px auto 40px; padding: 0 20px; }

.profile-toolbar {
  display: flex; gap: 0; margin-bottom: 20px; border-bottom: 2px solid #e5e9ef;
}
.profile-toolbar button {
  padding: 10px 28px; border: none; background: none; font-size: 15px;
  color: #666; cursor: pointer; position: relative; transition: color 0.2s;
}
.profile-toolbar button.active {
  color: #00a1d6; font-weight: 600;
}
.profile-toolbar button.active::after {
  content: ''; position: absolute; bottom: -2px; left: 0; right: 0;
  height: 2px; background: #00a1d6;
}

.home-view { }

.profile-header { display: flex; gap: 24px; background: #fff; padding: 24px; border-radius: 8px; box-shadow: 0 1px 3px rgba(0,0,0,0.08); }
.profile-avatar img { width: 100px; height: 100px; border-radius: 50%; object-fit: cover; }
.profile-info { flex: 1; }
.profile-info h2 { margin: 0 0 4px; font-size: 22px; }
.bio { color: #555; font-size: 14px; margin: 8px 0; }
.profile-meta { display: flex; flex-wrap: wrap; gap: 20px; font-size: 13px; color: #666; margin: 12px 0; }
.profile-actions { display: flex; gap: 10px; margin-top: 12px; }
.btn-follow { padding: 8px 24px; border-radius: 20px; border: 1px solid #00a1d6; background: #fff; color: #00a1d6; cursor: pointer; font-size: 14px; }
.btn-follow.following { background: #00a1d6; color: #fff; }
.btn-message { padding: 8px 24px; border-radius: 20px; border: 1px solid #ffa500; background: #fff; color: #ffa500; cursor: pointer; font-size: 14px; }
.btn-message:hover { background: #ffa500; color: #fff; }

.profile-videos { margin-top: 20px; }
.profile-videos h4 { margin-bottom: 16px; }
.video-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(180px, 1fr)); gap: 16px; }
.video-card { text-decoration: none; color: inherit; }
.thumb { position: relative; border-radius: 6px; overflow: hidden; aspect-ratio: 16/9; background: #eee; }
.thumb img { width: 100%; height: 100%; object-fit: cover; }
.duration { position: absolute; bottom: 4px; right: 4px; background: rgba(0,0,0,0.7); color: #fff; padding: 1px 6px; border-radius: 3px; font-size: 12px; }
.video-card .title { font-size: 13px; margin: 6px 0 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: #333; }
.image-count { position: absolute; bottom: 4px; right: 4px; background: rgba(0,0,0,0.7); color: #fff; padding: 1px 8px; border-radius: 10px; font-size: 12px; }

.empty-tip { text-align: center; padding: 60px 20px; background: #fff; border-radius: 8px; margin-top: 20px; }

.works-view { display: flex; gap: 24px; margin-top: 16px; }
.works-sidebar {
  width: 180px; flex-shrink: 0; background: #fff; border-radius: 8px;
  box-shadow: 0 1px 3px rgba(0,0,0,0.08); overflow: hidden;
}
.works-sidebar div {
  padding: 14px 20px; cursor: pointer; font-size: 14px; color: #333;
  border-left: 3px solid transparent; transition: all 0.2s;
}
.works-sidebar div:hover { background: #f5f7fa; }
.works-sidebar div.active {
  background: #e8f5fe; color: #00a1d6; border-left-color: #00a1d6; font-weight: 600;
}
.works-content { flex: 1; min-width: 0; }
.works-content h4 { margin-bottom: 16px; padding-bottom: 12px; border-bottom: 1px solid #eee; }

.gallery-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 16px; }
.gallery-card { text-decoration: none; color: inherit; }
.gallery-card .title { font-size: 13px; margin: 6px 0 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: #333; }

.article-list { display: flex; flex-direction: column; gap: 12px; }
.article-card {
  display: flex; gap: 16px; text-decoration: none; color: inherit;
  padding: 12px; border-radius: 8px; transition: background 0.2s;
}
.article-card:hover { background: #f5f7fa; }
.article-cover { width: 160px; flex-shrink: 0; border-radius: 6px; overflow: hidden; }
.article-cover img { width: 100%; height: 90px; object-fit: cover; }
.article-info { flex: 1; min-width: 0; }
.article-info h5 { margin: 0 0 4px; font-size: 15px; color: #222; }

.self-ban-banner {
  background: #fef0f0; color: #c0392b; border: 1px solid #f5c6cb;
  padding: 10px 16px; border-radius: 8px; margin-bottom: 16px;
  display: flex; gap: 8px; align-items: center; font-size: 14px; line-height: 1.5;
}
.other-ban-banner {
  background: #f5f5f5; color: #888; border: 1px solid #e0e0e0;
  padding: 10px 16px; border-radius: 8px; margin-bottom: 16px;
  display: flex; gap: 8px; align-items: center; font-size: 14px;
}
</style>
