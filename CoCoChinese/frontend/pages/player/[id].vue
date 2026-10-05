<template>
  <div class="player-page">
    <div class="player-container">
      <div class="player-left">
          <div class="player-stage-wrap" ref="playerStageWrapRef" :class="{ 'window-fullscreen-active': windowFullscreen }">
          <div class="player-stage" ref="playerStageRef" :class="{ 'window-fullscreen': windowFullscreen }">
            <video ref="videoRef" class="plyr" playsinline webkit-playsinline controls controlsList="nodownload" disablePictureInPicture draggable="false" @contextmenu.prevent @dragstart.prevent :poster="media?.image_url || media?.imageUrl || ''">
            </video>
            <div v-if="mediaLoading" class="player-empty">
              <i class="fa fa-spinner fa-pulse fa-3x"></i>
              <p>正在加载...</p>
            </div>
            <div v-else-if="!media?.videoUrl && !hasStream" class="player-empty">
              <i class="fa fa-film fa-4x"></i>
              <p>视频地址未设置</p>
            </div>
            <div v-if="videoWatermark" class="watermark-overlay">
              <div class="watermark-text" ref="watermarkRef">{{ videoWatermark }}</div>
            </div>
            <DanmakuOverlay v-if="hasStream" :slug="slug" :player="player" />
            <div v-if="windowFullscreen" class="fs-exit-btn" @click="toggleWindowFullscreen">
              <i class="fa fa-compress"></i> 退出窗口全屏
            </div>

            <Transition name="upnext-fade">
              <div v-if="showUpNext" class="upnext-overlay" @mouseenter="pauseUpNextCountdown" @mouseleave="resumeUpNextCountdown">
                <div class="upnext-inner">
                  <div class="upnext-card upnext-card--primary" v-if="nextUpVideos[0]" @click="playUpNextVideo(0)">
                    <div class="upnext-countdown-ring">
                      <svg viewBox="0 0 100 100" class="countdown-svg">
                        <circle cx="50" cy="50" r="44" class="countdown-track" />
                        <circle cx="50" cy="50" r="44" class="countdown-progress" :style="{ strokeDashoffset: 276.46 * (1 - upNextCircleProgress) }" />
                      </svg>
                      <div class="countdown-center">
                        <span class="countdown-number">{{ upNextCountdown }}</span>
                      </div>
                    </div>
                    <span class="upnext-badge">即将播放</span>
                    <div class="upnext-card-thumb">
                      <img :src="nextUpVideos[0].imageUrl || nextUpVideos[0].image_url || '/images/videoImg.webp'" alt="" loading="lazy">
                      <span class="upnext-card-duration">{{ nextUpVideos[0].videoTime || '' }}</span>
                      <div class="upnext-card-play"><i class="fa fa-play"></i></div>
                    </div>
                    <div class="upnext-card-info">
                      <p class="upnext-card-title">{{ nextUpVideos[0].title }}</p>
                      <div class="upnext-card-meta">
                        <span><i class="fa fa-user-o"></i> {{ nextUpVideos[0].author }}</span>
                        <span><i class="fa fa-eye"></i> {{ formatCount(nextUpVideos[0].watchVolue) }}</span>
                      </div>
                    </div>
                  </div>

                  <div class="upnext-side">
                    <div class="upnext-side-label">备选推荐</div>
                    <div v-for="(v, i) in upNextSecondaryVideos" :key="v.id" class="upnext-card upnext-card--secondary" @click="playUpNextVideo(i + 1)">
                      <div class="upnext-card-thumb">
                        <img :src="v.imageUrl || v.image_url || '/images/videoImg.webp'" alt="" loading="lazy">
                        <span class="upnext-card-duration">{{ v.videoTime || '' }}</span>
                        <div class="upnext-card-play"><i class="fa fa-play"></i></div>
                      </div>
                      <div class="upnext-card-info">
                        <p class="upnext-card-title">{{ v.title }}</p>
                        <div class="upnext-card-meta">
                          <span><i class="fa fa-user-o"></i> {{ v.author }}</span>
                          <span><i class="fa fa-eye"></i> {{ formatCount(v.watchVolue) }}</span>
                        </div>
                      </div>
                    </div>
                  </div>
                </div>

                <div class="upnext-footer">
                  <button class="upnext-btn" @click="cancelUpNext">
                    <i class="fa fa-times"></i> 取消
                  </button>
                </div>
              </div>
            </Transition>
          </div>

        </div>

        <div class="resolution-bar" v-if="resolutions.length > 0">
          <span class="res-label">画质：</span>
          <button v-for="r in resolutions" :key="r.key" class="res-btn" :class="{ active: currentRes === r.key }" @click="switchResolution(r.key)">
            {{ r.label }}
          </button>
        </div>

        <div class="fullscreen-controls">
          <button class="fs-btn" @click="toggleWindowFullscreen">
            <i class="fa" :class="windowFullscreen ? 'fa-compress' : 'fa-expand'"></i>
            {{ windowFullscreen ? '退出窗口全屏' : '窗口全屏' }}
          </button>
          <button class="fs-btn" @click="toggleWebFullscreen">
            <i class="fa fa-arrows-alt"></i> 网页全屏
          </button>
        </div>

        <div class="video-meta">
          <h1 class="video-title">{{ media?.title || '视频标题' }}</h1>
          <div class="stat-row">
            <span><i class="fa fa-eye"></i> {{ watchCount }}</span>
            <button class="btn-like" :class="{ liked: liked }" @click="toggleLike" :disabled="!user">
              <i class="fa" :class="liked ? 'fa-heart' : 'fa-heart-o'"></i> {{ likeCount }}
            </button>
            <span><i class="fa fa-clock-o"></i> {{ media?.videoTime || '00:00' }}</span>
          </div>
        </div>

        <div class="video-desc-box" v-if="media?.introduction || media?.description">
          <div class="desc-header">简介</div>
          <div class="desc-content" v-html="renderLinks(media?.introduction || media?.description || '')"></div>
        </div>

        <div class="comments-section">
          <div class="comments-header">
            <h3><i class="fa fa-comments me-2"></i>评论 ({{ comments.length }})</h3>
          </div>

          <div class="comment-form" v-if="user">
            <div class="comment-form-avatar">
              <img :src="user.avatar_url || '/images/default_avatar.png'" alt="">
            </div>
            <div class="comment-form-input">
              <textarea v-model="newComment" rows="3" placeholder="发一条友善的评论..." maxlength="500"></textarea>
              <div class="comment-form-actions">
                <div class="comment-toolbar">
                  <button class="btn btn-sm btn-outline-secondary me-1" @click="insertVideoLink" title="插入视频链接">
                    <i class="fa fa-video-camera"></i>
                  </button>
                  <button class="btn btn-sm btn-outline-secondary" @click="uploadCommentImage" title="上传图片">
                    <i class="fa fa-image"></i>
                  </button>
                  <input ref="commentImageInput" type="file" accept="image/*" class="d-none" @change="onCommentImageSelected">
                  <span v-if="commentImages.length" class="small text-muted ms-1">{{ commentImages.length }}张图</span>
                </div>
                <div class="comment-form-right">
                  <span class="text-muted small me-2">{{ newComment.length }}/500</span>
                  <button class="btn btn-primary btn-sm" @click="postComment" :disabled="!newComment.trim() || posting || isBanned">{{ posting ? '发布中...' : '发布' }}</button>
                </div>
              </div>
              <div v-if="commentImages.length" class="comment-image-previews mt-2">
                <div v-for="(img, i) in commentImages" :key="i" class="comment-image-preview">
                  <img :src="img" alt="">
                  <button class="btn-close" @click="commentImages.splice(i, 1)"></button>
                </div>
              </div>
            </div>
          </div>
          <div v-if="showVideoLinkInput" class="video-link-input p-2 border-top">
            <div class="input-group input-group-sm">
              <input v-model="videoLinkId" class="form-control" placeholder="输入视频ID" @keyup.enter="confirmVideoLink">
              <button class="btn btn-outline-primary" @click="confirmVideoLink" :disabled="checkingVideo">
                {{ checkingVideo ? '验证中...' : '确定' }}
              </button>
              <button class="btn btn-outline-secondary" @click="showVideoLinkInput = false">取消</button>
            </div>
            <div v-if="videoLinkError" class="small text-danger mt-1">{{ videoLinkError }}</div>
          </div>
          <div v-if="!user" class="comment-login-tip">
            <NuxtLink to="/login">登录</NuxtLink> 后发表评论
          </div>

          <div class="comments-list">
            <div v-for="item in rootComments" :key="item.id" class="comment-item">
              <div class="comment-avatar">
                <img :src="item.user_avatar || item.avatar_url || '/images/default_avatar.png'" alt="">
              </div>
              <div class="comment-body">
                <div class="comment-user">{{ item.user_display_name || item.username }}</div>
                <div class="comment-time">{{ formatTime(item.created_at) }}</div>
                <div class="comment-content" v-html="renderCommentContent(item.content)"></div>
                <div v-if="item.images?.length" class="comment-images mt-2">
                  <img v-for="(img, i) in item.images" :key="i" :src="img" class="comment-image" @click="previewImage(img)">
                </div>
                <button class="comment-reply-btn" @click="replyTo = item">回复</button>
                <div v-if="replyTo?.id === item.id" class="reply-form">
                  <textarea v-model="replyText" rows="2" placeholder="回复..." maxlength="300"></textarea>
                  <div class="reply-actions">
                    <button class="btn btn-link btn-sm text-muted" @click="replyTo = null">取消</button>
                    <button class="btn btn-primary btn-sm" @click="postReply(item)" :disabled="!replyText.trim()">回复</button>
                  </div>
                </div>
                <div v-if="getReplies(item.id).length" class="replies">
                  <div v-for="r in getReplies(item.id)" :key="r.id" class="reply-item">
                    <img :src="r.user_avatar || r.avatar_url || '/images/default_avatar.png'" alt="" class="reply-avatar">
                    <div class="reply-body">
                      <span class="reply-user">{{ r.user_display_name || r.username }}</span>
                      <span class="reply-time">{{ formatTime(r.created_at) }}</span>
                      <p class="reply-content" v-html="renderCommentContent(r.content)"></p>
                      <div v-if="r.images?.length" class="reply-images mt-1">
                        <img v-for="(img, i) in r.images" :key="i" :src="img" class="comment-image" @click="previewImage(img)">
                      </div>
                    </div>
                </div>
              </div>
            </div>
            </div>
            <div v-if="!comments.length" class="empty-comments">
              <i class="fa fa-comment-o fa-3x text-muted mb-2"></i>
              <p class="text-muted">还没有评论，快来抢沙发吧~</p>
            </div>
          </div>
        </div>
      </div>

      <div class="player-right" ref="rightPanelRef">
          <div class="up-panel-container">
            <div class="up-info-container">
              <div class="up-info--left">
                <div class="up-avatar-wrap">
                  <NuxtLink :to="'/user/' + authorSlug" class="up-avatar">
                    <img :src="media?.authorImg || '/images/default_avatar.png'" alt="" class="up-avatar-img">
                  </NuxtLink>
                </div>
              </div>
              <div class="up-info--right">
                <div class="up-info__detail">
                  <div class="up-detail">
                    <div class="up-detail-top">
                      <NuxtLink :to="'/user/' + authorSlug" class="up-name">{{ authorDisplayName || media?.author || 'UP主' }}</NuxtLink>
                    </div>
                    <div class="up-description">{{ authorDesc }}</div>
                  </div>
                </div>
                <div class="up-info__btn-panel">
                  <div class="upinfo-btn-panel">
                    <button v-if="authorId && (!user || user.id !== authorId)"
                      class="default-btn follow-btn" :class="{ following: isFollowing }" @click="toggleFollow">
                      <svg width="16" height="16" viewBox="0 0 16 16" fill="none" xmlns="http://www.w3.org/2000/svg" class="follow-btn-icon">
                        <path fill-rule="evenodd" clip-rule="evenodd" d="M7.25098 8.75V13.25C7.25098 13.6642 7.58676 14 8.00098 14C8.41519 14 8.75098 13.6642 8.75098 13.25V8.75H13.251C13.6652 8.75 14.001 8.41421 14.001 8C14.001 7.58579 13.6652 7.25 13.251 7.25H8.75098V2.75C8.75098 2.33579 8.41519 2 8.00098 2C7.58676 2 7.25098 2.33579 7.25098 2.75V7.25H2.75098C2.33676 7.25 2.00098 7.58579 2.00098 8C2.00098 8.41421 2.33676 8.75 2.75098 8.75H7.25098Z" fill="currentColor"></path>
                      </svg>
                      {{ isFollowing ? '已关注' : '关注' }}
                    </button>
                    <button v-if="authorId && (!user || user.id !== authorId)" class="send-msg" @click="openDM">
                      <i class="fa fa-commenting"></i> 发消息
                    </button>
                  </div>
                </div>
              </div>
            </div>
          </div>

        <div class="right-inner">
          <div class="danmaku-box">
            <div class="danmaku-header" @click="danmakuExpanded = !danmakuExpanded">
              <span class="danmaku-title">弹幕列表</span>
              <span class="danmaku-header-right">
                <span class="danmaku-count" v-if="danmakuList.length">{{ danmakuList.length }}条</span>
                <i class="fa" :class="danmakuExpanded ? 'fa-chevron-up' : 'fa-chevron-down'"></i>
              </span>
            </div>
            <div class="danmaku-list" ref="danmakuListRef" v-show="danmakuExpanded">
              <div v-for="d in danmakuList" :key="d.id" class="danmaku-item">
                <span class="dm-time">[{{ formatDanmakuTime(d.time) }}]</span>
                <span class="dm-user">{{ d.username }}</span>
                <span class="dm-text" :style="{ color: visibleDanmakuColor(d.color) }">{{ d.content }}</span>
              </div>
              <div v-if="!danmakuList.length" class="dm-empty">
                <i class="fa fa-commenting-o"></i> 暂无弹幕
              </div>
            </div>
          </div>

          <div class="recommend-section" v-if="nextUpVideos.length">
            <div class="rec-title">
              <span class="title-txt">接下来播放</span>
            </div>
            <div class="rec-list">
              <a v-for="v in nextUpVideos" :key="v.id" :href="'/player/' + v.slug" class="rec-card">
                <div class="rec-card-pic">
                  <img :src="v.imageUrl || v.image_url || '/images/videoImg.webp'" alt="" loading="lazy">
                  <span class="rec-duration">{{ v.videoTime || '' }}</span>
                </div>
                <div class="rec-card-info">
                  <p class="rec-card-title" :title="v.title">{{ v.title }}</p>
                  <div class="rec-card-meta">
                    <span class="rec-card-author"><i class="fa fa-user-o"></i> {{ v.author }}</span>
                    <span class="rec-card-views"><i class="fa fa-eye"></i> {{ formatCount(v.watchVolue) }}</span>
                  </div>
                </div>
              </a>
            </div>
          </div>

          <div class="recommend-section" v-if="recommendedVideos.length">
            <div class="rec-title">
              <span class="title-txt">推荐你喜欢的</span>
            </div>
            <div class="rec-list">
              <a v-for="v in recommendedVideos" :key="v.id" :href="'/player/' + v.slug" class="rec-card">
                <div class="rec-card-pic">
                  <img :src="v.imageUrl || v.image_url || '/images/videoImg.webp'" alt="" loading="lazy">
                  <span class="rec-duration">{{ v.videoTime || '' }}</span>
                </div>
                <div class="rec-card-info">
                  <p class="rec-card-title" :title="v.title">{{ v.title }}</p>
                  <div class="rec-card-meta">
                    <span class="rec-card-author"><i class="fa fa-user-o"></i> {{ v.author }}</span>
                    <span class="rec-card-views"><i class="fa fa-eye"></i> {{ formatCount(v.watchVolue) }}</span>
                  </div>
                </div>
              </a>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>

  <Teleport to="body">
    <div v-if="previewImageUrl" class="photo-viewer-overlay" @click.self="previewImageUrl = ''" @wheel.prevent="onPhotoWheel">
      <button class="photo-viewer-close" @click="previewImageUrl = ''">&times;</button>
      <div class="photo-zoom-bar">
        <button @click="photoZoomOut" :disabled="photoZoom <= 0.25" title="缩小"><i class="fa fa-search-minus"></i></button>
        <span class="photo-zoom-level">{{ Math.round(photoZoom * 100) }}%</span>
        <button @click="photoZoomIn" :disabled="photoZoom >= 5" title="放大"><i class="fa fa-search-plus"></i></button>
        <button @click="photoZoomReset" v-if="photoZoom !== 1" title="重置">1:1</button>
      </div>
      <img :src="previewImageUrl" class="photo-viewer-image" :style="{ transform: 'scale(' + photoZoom + ')' }" @click.self="previewImageUrl = ''">
    </div>
  </Teleport>
</template>

<script setup>
import { ref, computed, watch, onMounted, onUnmounted, nextTick } from 'vue'

const route = useRoute()
const videoRef = ref(null)
const player = ref(null)
const hlsInstance = ref(null)
const HlsClass = ref(null)
const hasStream = ref(false)
const watermarkRef = ref(null)
const rightPanelRef = ref(null)
const danmakuListRef = ref(null)
const danmakuExpanded = ref(false)
const devtoolsDisabled = ref(route.query.devtoolscheck === 'disable')

const playerStageRef = ref(null)
const playerStageWrapRef = ref(null)
const windowFullscreen = ref(false)

const { renderLinks } = useSafeLinks()
const fmp4 = useFMP4()

const slug = route.params.id
const media = ref(null)
const mediaLoading = ref(true)
const useFmp4 = ref(false)

const videoWatermark = ref('')

async function loadMedia() {
  mediaLoading.value = true
  try {
    const { negotiateSession, getSessionId, decryptUrls } = useVideoCrypto()
    const sid = await negotiateSession()
    let url = `/api/player/${slug}`
    if (sid) url += `?sessionId=${sid}`
    const data = await $fetch(url)
    media.value = data._enc ? await decryptUrls(data) : data
  } catch {
    try {
      const data = await $fetch(`/api/player/${slug}`)
      media.value = data
    } catch (e) {
      console.error('loadMedia failed:', e)
    }
  } finally {
    mediaLoading.value = false
  }
}

async function loadWatermark() {
  try {
    const u = await $fetch('/api/user/me', { default: () => ({ user: null }), transform: r => r.user || null })
    if (u) {
      videoWatermark.value = u.display_name || u.username || ''
    }
  } catch {}
}

const videoToken = ref('')
async function fetchVideoToken() {
  if (!slug) return
  try {
    const res = await $fetch(`/api/videos/token/${slug}`, { method: 'GET' })
    if (res.success && res.videoUrl) {
      videoToken.value = res.token
    }
  } catch {}
}

const resolutions = computed(() => {
  const list = []
  if (media.value?.videoUrl360p) list.push({ key: '360p', label: '360P' })
  if (media.value?.videoUrl720p) list.push({ key: '720p', label: '720P' })
  if (media.value?.videoUrl1080p) list.push({ key: '1080p', label: '1080P' })
  return list
})

const currentRes = ref('')

function getResUrl(res) {
  if (!media.value) return ''
  switch (res) {
    case '360p': return media.value.videoUrl360p
    case '720p': return media.value.videoUrl720p
    case '1080p': return media.value.videoUrl1080p
    default: return media.value.videoUrl || ''
  }
}

function appendToken(url) {
  if (!url) return ''
  return url + (url.includes('?') ? '&' : '?') + '_vt=' + encodeURIComponent(videoToken.value)
}

async function switchResolution(res) {
  if (res === currentRes.value || !videoToken.value) return

  const wasPlaying = player.value?.playing
  const currentTime = player.value?.currentTime || 0

  currentRes.value = res

  if (useFmp4.value) {
    await fmp4.switchResolution(res)
    if (videoRef.value) {
      videoRef.value.currentTime = currentTime
      if (wasPlaying) videoRef.value.play().catch(() => {})
    }
    return
  }

  const url = getResUrl(res)
  if (!url) return

  const hlsUrl = appendToken(url)

  if (hlsInstance.value) {
    hlsInstance.value.loadSource(hlsUrl)
    hlsInstance.value.once(HlsClass.value.Events.MANIFEST_PARSED, () => {
      if (videoRef.value) {
        videoRef.value.currentTime = currentTime
        if (wasPlaying) videoRef.value.play().catch(() => {})
      }
    })
  } else {
    await initHLS(hlsUrl)
    await nextTick()
    if (wasPlaying && player.value) {
      player.value.on('ready', () => { player.value.currentTime = currentTime }, { once: true })
    }
  }
}

const { data: comments, refresh: refreshComments } = await useFetch(`/api/comments/${slug}`, {
  default: () => [],
  transform: r => (Array.isArray(r) ? r : r?.data || []).map((c) => ({
    ...c,
    images: typeof c.images === 'string' ? (c.images ? JSON.parse(c.images) : []) : (c.images || [])
  }))
})
const { data: userData } = await useFetch('/api/user/me', { default: () => ({ user: null }), transform: r => r.user || null })
const user = computed(() => userData.value)

const newComment = ref('')
const posting = ref(false)
const replyTo = ref(null)
const replyText = ref('')
const commentImages = ref([])
const commentImageInput = ref()
const showVideoLinkInput = ref(false)
const videoLinkId = ref('')
const videoLinkError = ref('')
const checkingVideo = ref(false)

function renderCommentContent(text) {
  return text.replace(/\[video:([^\]]+)\]([^\[]*)\[\/video\]/g, (_, id, title) => {
    return `<a href="/player/${id}" target="_blank" class="video-link"><i class="fa fa-video-camera me-1"></i>${title || id}</a>`
  })
}

async function insertVideoLink() {
  showVideoLinkInput.value = !showVideoLinkInput.value
  videoLinkId.value = ''
  videoLinkError.value = ''
  if (showVideoLinkInput.value) {
    await nextTick()
    // focus the input
  }
}

async function confirmVideoLink() {
  const id = videoLinkId.value.trim()
  if (!id) { videoLinkError.value = '请输入视频ID'; return }
  checkingVideo.value = true
  videoLinkError.value = ''
  try {
    const res = await $fetch(`/api/videos/check/${id}`)
    if (res.success) {
      const title = res.data.title
      const link = `[video:${id}]${title}[/video]`
      const pos = newComment.value.length
      newComment.value = newComment.value.slice(0, pos) + ' ' + link + ' ' + newComment.value.slice(pos)
      showVideoLinkInput.value = false
    }
  } catch (e) {
    videoLinkError.value = e?.data?.message || '视频不存在或已被屏蔽'
  } finally {
    checkingVideo.value = false
  }
}

async function uploadCommentImage() {
  commentImageInput.value?.click()
}

async function onCommentImageSelected(e) {
    const input = e.target
  const file = input?.files?.[0]
  if (!file) return
  const fd = new FormData()
  fd.append('image', file)
  try {
    const res = await $fetch('/api/upload/comment-image', { method: 'POST', body: fd })
    if (res.success) commentImages.value.push(res.url)
  } catch {
    alert('图片上传失败')
  }
  input.value = ''
}

const previewImageUrl = ref('')
const photoZoom = ref(1)

function previewImage(url) {
  previewImageUrl.value = url
  photoZoom.value = 1
}

function onPhotoViewerKeydown(e) {
  if (e.key === 'Escape') previewImageUrl.value = ''
}

function photoZoomIn() {
  photoZoom.value = Math.min(photoZoom.value + 0.25, 5)
}

function photoZoomOut() {
  photoZoom.value = Math.max(photoZoom.value - 0.25, 0.25)
}

function photoZoomReset() {
  photoZoom.value = 1
}

function onPhotoWheel(e) {
  if (e.deltaY < 0) photoZoomIn()
  else photoZoomOut()
  e.preventDefault()
}

watch(previewImageUrl, (val) => {
  if (val) document.addEventListener('keydown', onPhotoViewerKeydown)
  else document.removeEventListener('keydown', onPhotoViewerKeydown)
})

const rootComments = computed(() => comments.value.filter(c => !c.parent_id))
function getReplies(parentId) { return comments.value.filter(c => c.parent_id === parentId) }

const liked = ref(false)
const likeCount = ref(parseInt(media.value?.likeVolue || '0'))
const watchCount = ref(parseInt(media.value?.watchVolue || '0'))

const authorSlug = ref(null)
const authorId = ref(null)
const authorDesc = ref('')
const authorDisplayName = ref('')

async function fetchAuthorInfo() {
  const author = media.value?.author
  if (!author) return
  try {
    const res = await $fetch(`/api/user/${encodeURIComponent(author)}`, { method: 'GET' })
    if (res.success) {
      authorSlug.value = res.profile.slug || res.profile.username
      authorId.value = res.profile.id
      authorDesc.value = res.profile.description || res.profile.bio || ''
      authorDisplayName.value = res.profile.display_name || ''
    }
  } catch (e) {
    console.error('fetchAuthorInfo failed:', e)
  }
}

watch(media, (val) => {
  if (val?.author && !authorId.value) {
    fetchAuthorInfo()
  }
})

async function fetchLikeStatus() {
  if (!user.value || !media.value?.id) return
  try {
    const res = await $fetch(`/api/videos/${media.value.id}/like`, { method: 'GET' })
    liked.value = res.liked
  } catch {}
}

const isFollowing = ref(false)
async function toggleFollow() {
  if (!user.value) { navigateTo('/login'); return }
  if (!authorId.value) return
  try {
    const res = await $fetch(`/api/user/${authorId.value}/follow`, { method: 'POST' })
    isFollowing.value = res.following
  } catch (e) { alert(e.data?.message || '操作失败') }
}

async function toggleLike() {
  if (!user.value) { navigateTo('/login'); return }
  if (!media.value?.id) return
  try {
    const res = await $fetch(`/api/videos/${media.value.id}/like`, { method: 'POST' })
    liked.value = res.liked
    likeCount.value = res.likeVolue
  } catch (e) { alert(e.data?.message || '操作失败') }
}

async function recordView() {
  if (!media.value?.id) return
  try {
    const res = await $fetch(`/api/videos/${media.value.id}/view`, { method: 'POST' })
    watchCount.value = res.watchVolue
  } catch {}
}

function openDM() {
  if (!user.value) { navigateTo('/login'); return }
  if (!authorId.value) return
  navigateTo('/messages?dm=' + authorId.value)
}

function formatTime(t) {
  if (!t) return ''
  const d = new Date(t)
  const now = new Date()
  const diff = (now - d) / 1000
  if (diff < 60) return '刚刚'
  if (diff < 3600) return Math.floor(diff / 60) + '分钟前'
  if (diff < 86400) return Math.floor(diff / 3600) + '小时前'
  if (diff < 2592000) return Math.floor(diff / 86400) + '天前'
  return d.toLocaleDateString('zh-CN')
}

function formatDanmakuTime(t) {
  const m = Math.floor(t / 60)
  const s = Math.floor(t % 60)
  return String(m).padStart(2, '0') + ':' + String(s).padStart(2, '0')
}

function visibleDanmakuColor(color) {
  if (!color || color.toLowerCase() === '#ffffff' || color.toLowerCase() === '#fff' || color.toLowerCase() === 'white') return '#333'
  return color
}

function formatCount(n) {
  if (!n) return '0'
  const num = parseInt(n)
  if (num >= 10000) return (num / 10000).toFixed(1) + '万'
  return String(num)
}

async function postComment() {
  if (!newComment.value.trim() || posting.value) return
  posting.value = true
  try {
    await $fetch(`/api/comments/${slug}`, {
      method: 'POST',
      body: { content: newComment.value.trim(), images: commentImages.value }
    })
    newComment.value = ''
    commentImages.value = []
    await refreshComments()
  } catch {
    alert('评论失败')
  } finally { posting.value = false }
}

async function postReply(parent) {
  if (!replyText.value.trim()) return
  try {
    await $fetch(`/api/comments/${slug}`, {
      method: 'POST', body: { content: replyText.value.trim(), parent_id: parent.id }
    })
    replyText.value = ''
    replyTo.value = null
    await refreshComments()
  } catch { alert('回复失败') }
}

function attachVideoSource(url) {
  if (!videoRef.value) return
  while (videoRef.value.firstChild) {
    videoRef.value.removeChild(videoRef.value.firstChild)
  }
  const source = document.createElement('source')
  source.src = url
  videoRef.value.appendChild(source)
  videoRef.value.load()
}

async function initHLS(hlsUrl) {
  if (!videoRef.value || !hlsUrl) return

  if (!hlsUrl.includes('.m3u8')) {
    hasStream.value = true
    await nextTick()
    attachVideoSource(hlsUrl)
    await initPlyr()
    return
  }

  try {
    const Hls = (await import('hls.js')).default
    HlsClass.value = Hls
    if (!Hls.isSupported()) {
      const mp4Url = appendToken(media.value?.videoUrl || '')
      if (mp4Url) { attachVideoSource(mp4Url); await initPlyr() }
      return
    }
    destroyHLS()
    hasStream.value = true
    hlsInstance.value = new Hls({ enableWorker: true, lowLatencyMode: false })
    hlsInstance.value.loadSource(hlsUrl)
    hlsInstance.value.attachMedia(videoRef.value)
    hlsInstance.value.on(Hls.Events.MANIFEST_PARSED, () => { initPlyr() })
    hlsInstance.value.on(Hls.Events.ERROR, (event, data) => {
      if (data.fatal) {
        switch (data.type) {
          case Hls.ErrorTypes.NETWORK_ERROR:
            hlsInstance.value?.startLoad()
            break
          case Hls.ErrorTypes.MEDIA_ERROR:
            hlsInstance.value?.recoverMediaError()
            break
          default:
            destroyHLS()
            const fallbackUrl = appendToken(media.value?.videoUrl || '')
            if (fallbackUrl) { attachVideoSource(fallbackUrl); initPlyr() }
            break
        }
      }
    })
  } catch {
    const fallbackUrl = appendToken(media.value?.videoUrl || '')
    if (fallbackUrl) { attachVideoSource(fallbackUrl); await initPlyr() }
  }
}

let plyrInitialized = false
let plyrInitializing = false
async function initPlyr() {
  if (plyrInitialized || plyrInitializing || !videoRef.value) return
  plyrInitializing = true
  plyrInitialized = true
  await nextTick()
  const Plyr = (await import('plyr')).default
  await import('plyr/dist/plyr.css')
  if (player.value) {
    try { player.value.destroy() } catch {}
    player.value = null
  }
  player.value = new Plyr(videoRef.value, {
    controls: ['play-large', 'play', 'progress', 'current-time', 'duration', 'mute', 'volume', 'settings'],
    settings: ['speed', 'loop'],
    i18n: {
      restart: '重新播放',
      rewind: '快退 {seektime} 秒',
      fastForward: '快进 {seektime} 秒',
      play: '播放',
      pause: '暂停',
      stop: '停止',
      seek: '搜索',
      played: '已播放',
      buffered: '缓冲中',
      currentTime: '当前时间',
      duration: '总时长',
      volume: '音量',
      mute: '静音',
      unmute: '取消静音',
      captions: '字幕',
      enableCaptions: '开启字幕',
      disableCaptions: '关闭字幕',
      enterFullscreen: '进入全屏',
      exitFullscreen: '退出全屏',
      frameTitle: '{title} - 播放器',
      settings: '设置',
      speed: '播放速度',
      normal: '正常',
      quality: '画质',
      loop: '循环播放',
      start: '起点',
      end: '终点',
      all: '全部',
      reset: '重置',
      disabled: '关闭',
      enabled: '开启',
      advertisement: '广告',
      qualityBadge: {
        2160: '4K',
        1440: '2K',
        1080: '1080P',
        720: '720P',
        480: '480P',
        360: '360P',
      },
    },
  })
  setupPlayerEvents()
}

function setupPlayerEvents() {
  if (!player.value) return
  hydrateProgress()
  player.value.on('playing', recordView, { once: true })
  player.value.on('ended', onVideoEnded)
  let saveTimer = null
  player.value.on('timeupdate', () => {
    if (saveTimer) clearTimeout(saveTimer)
    saveTimer = setTimeout(async () => {
      try {
        await $fetch('/api/history', {
          method: 'POST',
          body: { video_id: media.value?.id, progress: player.value?.currentTime || 0 }
        })
      } catch {}
    }, 5000)
  })
}

async function hydrateProgress() {
  if (!player.value) return
  try {
    const hist = await $fetch('/api/history', { params: { limit: 50 } })
    if (Array.isArray(hist)) {
      const entry = hist.find(h => h.slug === slug || h.video_id === media.value?.id)
      if (entry?.progress && entry.progress > 0) {
        player.value.on('ready', () => { player.value.currentTime = entry.progress })
      }
    }
  } catch {}
}

function destroyHLS() {
  if (hlsInstance.value) {
    try { hlsInstance.value.destroy() } catch {}
    hlsInstance.value = null
  }
}

function destroyPlayer() {
  if (useFmp4.value) {
    fmp4.destroy()
    useFmp4.value = false
  }
  if (player.value) {
    try { player.value.destroy() } catch {}
    player.value = null
  }
  plyrInitialized = false
}

const danmakuList = ref([])
let danmakuEventSource = null

async function fetchDanmaku() {
  try {
    const res = await $fetch(`/api/danmaku/${slug}`, { params: { segment: 0 } })
    if (res?.data) {
      danmakuList.value = res.data.slice(-100)
    }
  } catch {}
}

function connectDanmakuSSE() {
  danmakuEventSource = new EventSource(`/api/danmaku/${slug}/sse`)
  danmakuEventSource.onmessage = (e) => {
    try {
      const data = JSON.parse(e.data)
      if (data.type === 'danmaku' && data.danmaku) {
        const d = data.danmaku
        if (!danmakuList.value.find(item => item.id === d.id)) {
          danmakuList.value.push(d)
          if (danmakuList.value.length > 200) {
            danmakuList.value = danmakuList.value.slice(-100)
          }
        }
      }
    } catch {}
  }
}

const nextUpVideos = ref([])
const recommendedVideos = ref([])

const showUpNext = ref(false)
const upNextCountdown = ref(10)
const upNextCountdownTotal = 10
let upNextCountdownTimer = null
const upNextPaused = ref(false)

function onVideoEnded() {
  if (!nextUpVideos.value.length) return
  showUpNext.value = true
  upNextCountdown.value = upNextCountdownTotal
  upNextPaused.value = false
  startUpNextCountdown()
}

function startUpNextCountdown() {
  clearUpNextCountdown()
  upNextCountdownTimer = setInterval(() => {
    if (upNextPaused.value) return
    upNextCountdown.value--
    if (upNextCountdown.value <= 0) {
      clearUpNextCountdown()
      playUpNextVideo(0)
    }
  }, 1000)
}

function clearUpNextCountdown() {
  if (upNextCountdownTimer) {
    clearInterval(upNextCountdownTimer)
    upNextCountdownTimer = null
  }
}

function pauseUpNextCountdown() {
  upNextPaused.value = true
}

function resumeUpNextCountdown() {
  upNextPaused.value = false
}

function cancelUpNext() {
  showUpNext.value = false
  clearUpNextCountdown()
}

function playUpNextVideo(index) {
  clearUpNextCountdown()
  const v = nextUpVideos.value[index]
  if (v?.slug) {
    showUpNext.value = false
    navigateTo('/player/' + v.slug)
  }
}

const upNextPrimaryVideo = computed(() => nextUpVideos.value[0] || null)
const upNextSecondaryVideos = computed(() => nextUpVideos.value.slice(1, 3))

const upNextCircleProgress = computed(() => {
  const pct = (upNextCountdownTotal - upNextCountdown.value) / upNextCountdownTotal
  return pct
})

async function fetchRecommendations() {
  nextUpVideos.value = []
  recommendedVideos.value = []

  const { getSessionId, decryptUrls } = useVideoCrypto()
  const sid = getSessionId()
  const commonParams = { sort: 'popular', take: 8, exclude: slug }
  if (sid) commonParams.sessionId = sid

  try {
    let nextUp = []

    if (media.value?.author) {
      const params = { ...commonParams, author: media.value.author }
      const authorRes = await $fetch('/api/videos', { params })
      const data = sid ? await decryptUrls(authorRes) : authorRes
      if (data?.rows) {
        nextUp = data.rows.slice(0, 6)
      }
    }

    if (nextUp.length < 3 && media.value.video_type) {
      const params = { ...commonParams, category: media.value.video_type }
      const catRes = await $fetch('/api/videos', { params })
      const data = sid ? await decryptUrls(catRes) : catRes
      if (data?.rows) {
        const existingIds = new Set(nextUp.map(v => v.id))
        for (const v of data.rows) {
          if (nextUp.length >= 6) break
          if (!existingIds.has(v.id)) {
            nextUp.push(v)
            existingIds.add(v.id)
          }
        }
      }
    }

    if (nextUp.length < 3) {
      const genRes = await $fetch('/api/videos', { params: commonParams })
      const data = sid ? await decryptUrls(genRes) : genRes
      if (data?.rows) {
        const existingIds = new Set(nextUp.map(v => v.id))
        for (const v of data.rows) {
          if (nextUp.length >= 6) break
          if (!existingIds.has(v.id)) {
            nextUp.push(v)
            existingIds.add(v.id)
          }
        }
      }
    }

    nextUpVideos.value = nextUp
  } catch (e) {
    console.error('fetch next up failed:', e)
  }

  try {
    const popParams = { sort: 'popular', take: 12, exclude: slug }
    if (sid) popParams.sessionId = sid
    const popRes = await $fetch('/api/videos', { params: popParams })
    const popData = sid ? await decryptUrls(popRes) : popRes
    if (popData?.rows) {
      const excludedIds = new Set(nextUpVideos.value.map(v => v.id))
      recommendedVideos.value = popData.rows.filter(v => !excludedIds.has(v.id)).slice(0, 10)
    }
  } catch (e) {
    console.error('fetch recommended failed:', e)
  }
}

function preventDevTools(e) {
  if (
    e.key === 'F12' ||
    (e.ctrlKey && e.shiftKey && ['I', 'J', 'C'].includes(e.key)) ||
    (e.ctrlKey && e.key === 'U') ||
    (e.key === 'PrintScreen') ||
    (e.ctrlKey && e.key === 's') ||
    (e.metaKey && e.key === 's')
  ) {
    e.preventDefault()
    return false
  }
}

let devToolsInterval = null
let debuggerInterval = null
function detectDevTools() {
  const threshold = 160
  const w = window.outerWidth - window.innerWidth > threshold
  const h = window.outerHeight - window.innerHeight > threshold
  if (w || h) return true

  try {
    const e = new Error()
    if (e.stack && e.stack.includes('debugger')) return true
  } catch {}

  try {
    const img = new Image()
    Object.defineProperty(img, 'id', { get: () => { return true } })
    console.log(img)
  } catch { return true }

  return false
}

function startDevToolsDetection() {
  devToolsInterval = setInterval(() => {
    if (detectDevTools()) {
      document.title = '页面已被保护'
      document.body.innerHTML = ''
      window.location.replace('/')
    }
  }, 1000)

  debuggerInterval = setInterval(() => {
    const before = new Date()
    debugger
    const after = new Date()
    if (after - before > 100) {
      document.title = '页面已被保护'
      document.body.innerHTML = ''
      window.location.replace('/')
    }
  }, 3000)
}

let tokenRefreshTimer = null
function startTokenRefresh() {
  tokenRefreshTimer = setInterval(async () => {
    const oldToken = videoToken.value
    const oldRes = currentRes.value
    await fetchVideoToken()
    if (videoToken.value !== oldToken && oldRes) {
      if (useFmp4.value) {
        fmp4.setToken(videoToken.value)
      } else {
        const url = getResUrl(oldRes)
        if (url && hlsInstance.value) {
          const ct = player.value?.currentTime || 0
          const wasPlaying = player.value?.playing
          hlsInstance.value.loadSource(appendToken(url))
          if (HlsClass.value) {
            hlsInstance.value.once(HlsClass.value.Events.MANIFEST_PARSED, () => {
              if (videoRef.value) {
                videoRef.value.currentTime = ct
                if (wasPlaying) videoRef.value.play().catch(() => {})
              }
            })
          }
        }
      }
    }
  }, 25 * 60 * 1000)
}

function preventCopy(e) {
  e.preventDefault()
  return false
}

// --- Fullscreen ---
async function toggleWebFullscreen() {
  if (!playerStageRef.value) return
  if (windowFullscreen.value) {
    windowFullscreen.value = false
  }
  try {
    if (document.fullscreenElement) {
      await document.exitFullscreen()
    } else {
      await playerStageRef.value.requestFullscreen()
    }
  } catch {}
}

function toggleWindowFullscreen() {
  if (document.fullscreenElement) {
    document.exitFullscreen().catch(() => {})
  }
  if (windowFullscreen.value) {
    windowFullscreen.value = false
    document.body.style.overflow = ''
    return
  }
  windowFullscreen.value = true
  document.body.style.overflow = 'hidden'
  nextTick(() => {
    if (player.value && !player.value.paused) {
      try { player.value.play() } catch {}
    }
  })
}

onMounted(async () => {
  if (!devtoolsDisabled.value) {
    document.addEventListener('keydown', preventDevTools)
    document.addEventListener('copy', preventCopy)
    document.addEventListener('cut', preventCopy)
    startDevToolsDetection()
  }
  loadWatermark()

  await loadMedia()
  await fetchVideoToken()
  if (!videoToken.value) {
    console.error('Failed to obtain video access token')
  } else {
    const res = resolutions.value
    const preferred = res.length > 0 ? (res.find(r => r.key === '720p') || res[res.length - 1]) : null

    if (preferred) {
      currentRes.value = preferred.key
      await nextTick()
      const fmp4Ok = await fmp4.init(slug, videoToken.value, videoRef.value, preferred.key)
      if (fmp4Ok) {
        useFmp4.value = true
        hasStream.value = true
        await initPlyr()
      } else {
        const url = getResUrl(preferred.key)
        await initHLS(appendToken(url))
      }
    } else if (media.value?.videoUrlHls) {
      currentRes.value = 'hls'
      await initHLS(appendToken(media.value.videoUrlHls))
    } else if (media.value?.videoUrl) {
      currentRes.value = 'mp4'
      hasStream.value = true
      await nextTick()
      attachVideoSource(appendToken(media.value.videoUrl))
      await initPlyr()
    }
  }

  fetchLikeStatus()
  fetchAuthorInfo()
  recordView()
  startTokenRefresh()
  fetchDanmaku()
  fetchRecommendations()
  connectDanmakuSSE()

  document.addEventListener('fullscreenchange', onFullscreenChange)
})

onUnmounted(() => {
  destroyHLS()
  destroyPlayer()
  if (!devtoolsDisabled.value) {
    document.removeEventListener('keydown', preventDevTools)
    document.removeEventListener('copy', preventCopy)
    document.removeEventListener('cut', preventCopy)
    if (devToolsInterval) clearInterval(devToolsInterval)
    if (debuggerInterval) clearInterval(debuggerInterval)
  }
  if (tokenRefreshTimer) clearInterval(tokenRefreshTimer)
  if (danmakuEventSource) danmakuEventSource.close()
  clearUpNextCountdown()
  document.removeEventListener('fullscreenchange', onFullscreenChange)
})

function onFullscreenChange() {
  if (!document.fullscreenElement && windowFullscreen.value) {
    windowFullscreen.value = false
    document.body.style.overflow = ''
  }
}
const { banned: isBanned } = useBan()
</script>

<style scoped>
.player-page { background: #f5f5f5; min-height: 100vh; padding-top: 80px; }
.player-container { max-width: 1400px; margin: 0 auto; padding: 0 20px 40px; display: flex; gap: 20px; align-items: flex-start; }
.player-left { flex: 1; min-width: 0; }

.player-stage-wrap { position: relative; }

.player-stage { position: relative; width: 100%; background: #000; border-radius: 4px; overflow: hidden; user-select: none; -webkit-user-select: none; }
.player-stage video { width: 100%; display: block; aspect-ratio: 16 / 9; object-fit: contain; }
.player-stage video::-internal-media-controls-download-button { display: none; }
.player-stage video::-webkit-media-controls-download-button { display: none; }
.player-empty { position: absolute; inset: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; color: #888; gap: 12px; }
.watermark-overlay { position: absolute; top: 0; left: 0; right: 0; bottom: 0; pointer-events: none; z-index: 2; overflow: hidden; }
.watermark-text { position: absolute; bottom: 60px; right: 16px; color: rgba(255,255,255,0.35); font-size: 13px; font-weight: 500; text-shadow: 0 1px 3px rgba(0,0,0,0.5); user-select: none; pointer-events: none; white-space: nowrap; transform: rotate(-15deg); opacity: 0.7; }

/* Window Fullscreen */
.player-stage-wrap.window-fullscreen-active { min-height: 100vh; }
.player-stage.window-fullscreen {
  position: fixed !important; top: 0; left: 0; width: 100vw; height: 100vh;
  z-index: 9999; border-radius: 0;
}
.player-stage.window-fullscreen video { width: 100%; height: 100%; aspect-ratio: unset; }
.player-stage.window-fullscreen .plyr { height: 100%; }
.player-stage.window-fullscreen :deep(.plyr__video-wrapper) { height: 100%; }
.fs-exit-btn {
  position: fixed; top: 16px; right: 16px; z-index: 10000; background: rgba(0,0,0,0.5);
  color: #fff; border: none; border-radius: 4px; padding: 8px 16px; cursor: pointer; font-size: 14px;
}
.fs-exit-btn:hover { background: rgba(0,0,0,0.7); }

/* Mini Player */
.mini-player {
  position: fixed; bottom: 20px; right: 20px; width: 320px; z-index: 2000;
  border-radius: 8px; overflow: hidden; box-shadow: 0 4px 20px rgba(0,0,0,0.35);
  background: #000; cursor: pointer;
}
.mini-player-video-wrap { width: 100%; }
.mini-player-video-wrap video { width: 100%; display: block; aspect-ratio: 16 / 9; object-fit: contain; }
.mini-player-bar {
  display: flex; align-items: center; justify-content: space-between;
  padding: 6px 12px; background: #1a1a1a; color: #ccc; font-size: 12px;
}
.mini-player-title { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; margin-right: 8px; }
.mini-player-close {
  background: none; border: none; color: #999; font-size: 20px; cursor: pointer;
  padding: 0 4px; line-height: 1; transition: color 0.2s;
}
.mini-player-close:hover { color: #fff; }

.resolution-bar { background: #fff; padding: 8px 20px; border-top: 1px solid #f0f0f0; display: flex; align-items: center; gap: 8px; }
.res-label { font-size: 13px; color: #666; white-space: nowrap; }
.res-btn { padding: 4px 14px; border: 1px solid #ddd; border-radius: 4px; background: #fff; color: #333; font-size: 13px; cursor: pointer; transition: all 0.2s; }
.res-btn:hover { border-color: #00a1d6; color: #00a1d6; }
.res-btn.active { background: #00a1d6; color: #fff; border-color: #00a1d6; }

.fullscreen-controls {
  display: flex; gap: 8px; padding: 6px 20px; background: #fff;
  border-top: 1px solid #f0f0f0; border-bottom: 1px solid #f0f0f0;
}
.fs-btn {
  background: none; border: 1px solid #ddd; border-radius: 4px; padding: 4px 12px;
  font-size: 12px; color: #555; cursor: pointer; transition: all 0.2s;
  display: flex; align-items: center; gap: 4px;
}
.fs-btn:hover { border-color: #00a1d6; color: #00a1d6; background: #f8f9ff; }

.video-meta { background: #fff; padding: 20px; border-radius: 0 0 4px 4px; box-shadow: 0 1px 2px rgba(0,0,0,0.06); }
.video-title { font-size: 20px; font-weight: 600; margin: 0 0 16px; color: #222; }
.stat-row { display: flex; gap: 24px; color: #666; font-size: 14px; align-items: center; }
.stat-row span { white-space: nowrap; }
.btn-like { background: none; border: none; color: #666; cursor: pointer; font-size: 14px; padding: 0; }
.btn-like.liked { color: #fb7299; }
.btn-like:disabled { opacity: 0.5; cursor: not-allowed; }
.video-desc-box { background: #fff; padding: 16px 20px; margin-top: 12px; border-radius: 4px; box-shadow: 0 1px 2px rgba(0,0,0,0.06); }
.desc-header { font-weight: 600; font-size: 14px; color: #333; margin-bottom: 8px; }
.video-desc-box p { margin: 0; font-size: 14px; line-height: 1.8; color: #555; white-space: pre-wrap; }
.desc-content { margin: 0; font-size: 14px; line-height: 1.8; color: #555; white-space: pre-wrap; }
.desc-content :deep(.desc-link) { color: #1a73e8; text-decoration: none; }
.desc-content :deep(.desc-link:hover) { text-decoration: underline; }
.comments-section { margin-top: 16px; }
.comments-header { background: #fff; padding: 16px 20px; border-radius: 4px 4px 0 0; box-shadow: 0 1px 2px rgba(0,0,0,0.06); }
.comments-header h3 { margin: 0; font-size: 16px; font-weight: 600; }
.comment-form { display: flex; gap: 12px; background: #fff; padding: 16px 20px; border-top: 1px solid #f0f0f0; }
.comment-form-avatar img { width: 36px; height: 36px; border-radius: 50%; object-fit: cover; }
.comment-form-input { flex: 1; }
.comment-form-input textarea { width: 100%; border: 1px solid #e0e0e0; border-radius: 4px; padding: 10px; resize: none; font-size: 14px; outline: none; transition: border-color 0.2s; }
.comment-form-input textarea:focus { border-color: #00a1d6; }
.comment-form-actions { display: flex; justify-content: space-between; align-items: center; margin-top: 8px; }
.comment-login-tip { background: #fff; padding: 16px 20px; border-top: 1px solid #f0f0f0; text-align: center; color: #999; font-size: 14px; }
.comment-login-tip a { color: #00a1d6; text-decoration: none; font-weight: 500; }
.comments-list { background: #fff; border-radius: 0 0 4px 4px; box-shadow: 0 1px 2px rgba(0,0,0,0.06); }
.comment-item { display: flex; gap: 12px; padding: 16px 20px; border-top: 1px solid #f5f5f5; }
.comment-avatar img { width: 36px; height: 36px; border-radius: 50%; object-fit: cover; }
.comment-body { flex: 1; min-width: 0; }
.comment-user { font-weight: 600; font-size: 13px; color: #00a1d6; }
.comment-time { font-size: 12px; color: #999; margin: 2px 0 6px; }
.comment-content { font-size: 14px; line-height: 1.6; color: #333; word-break: break-word; }
.comment-reply-btn { background: none; border: none; color: #999; font-size: 12px; cursor: pointer; padding: 4px 0; }
.comment-reply-btn:hover { color: #00a1d6; }
.reply-form { margin-top: 8px; }
.reply-form textarea { width: 100%; border: 1px solid #e0e0e0; border-radius: 4px; padding: 8px; resize: none; font-size: 13px; outline: none; }
.reply-form textarea:focus { border-color: #00a1d6; }
.reply-actions { display: flex; justify-content: flex-end; gap: 8px; margin-top: 6px; }
.replies { margin-top: 10px; }
.reply-item { display: flex; gap: 10px; padding: 8px 0; }
.reply-avatar { width: 28px; height: 28px; border-radius: 50%; object-fit: cover; }
.reply-body { flex: 1; }
.reply-user { font-weight: 600; font-size: 12px; color: #00a1d6; }
.reply-time { font-size: 11px; color: #999; margin-left: 8px; }
.reply-content { font-size: 13px; line-height: 1.5; color: #333; margin: 2px 0 0; }
.empty-comments { text-align: center; padding: 40px 20px; }

/* Right Panel */
.player-right { width: 300px; flex-shrink: 0; position: sticky; top: 80px; max-height: calc(100vh - 100px); display: flex; flex-direction: column; overflow: hidden; }
.player-right::-webkit-scrollbar { width: 4px; }
.player-right::-webkit-scrollbar-thumb { background: #ccc; border-radius: 2px; }

.up-panel-container { flex-shrink: 0; background: #fff; border-radius: 4px; box-shadow: 0 1px 2px rgba(0,0,0,0.06); padding: 16px; }

.right-inner { display: flex; flex-direction: column; gap: 12px; overflow-y: auto; flex: 1; }
.right-inner::-webkit-scrollbar { width: 4px; }
.right-inner::-webkit-scrollbar-thumb { background: #ccc; border-radius: 2px; }
.up-info-container { display: flex; gap: 14px; }
.up-info--left { flex-shrink: 0; }
.up-avatar-wrap { position: relative; }
.up-avatar { display: block; }
.up-avatar-img { width: 48px; height: 48px; border-radius: 50%; object-fit: cover; }
.up-info--right { flex: 1; min-width: 0; }
.up-info__detail { margin-bottom: 10px; }
.up-detail-top { display: flex; align-items: center; justify-content: space-between; gap: 8px; margin-bottom: 4px; }
.up-name { font-size: 15px; font-weight: 600; color: #222; text-decoration: none; }
.up-name:hover { color: #00a1d6; }
.send-msg { font-size: 12px; color: #00a1d6; background: none; border: 1px solid #00a1d6; border-radius: 4px; padding: 2px 8px; cursor: pointer; white-space: nowrap; }
.send-msg:hover { background: #00a1d6; color: #fff; }
.up-description { font-size: 12px; color: #999; line-height: 1.5; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.up-info__btn-panel { margin-top: 4px; }
.upinfo-btn-panel { display: flex; gap: 8px; }
.default-btn { display: inline-flex; align-items: center; gap: 4px; padding: 5px 12px; border-radius: 4px; font-size: 12px; cursor: pointer; border: 1px solid #e0e0e0; background: #fff; color: #555; transition: all 0.2s; }
.default-btn:hover { border-color: #00a1d6; color: #00a1d6; }
.follow-btn.following { background: #00a1d6; border-color: #00a1d6; color: #fff; }
.follow-btn-icon { width: 14px; height: 14px; }

.danmaku-box { background: #fff; border-radius: 4px; box-shadow: 0 1px 2px rgba(0,0,0,0.06); padding: 12px 14px; }
.danmaku-header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 10px; padding-bottom: 8px; border-bottom: 1px solid #f0f0f0; cursor: pointer; user-select: none; }
.danmaku-header:hover { opacity: 0.8; }
.danmaku-title { font-size: 14px; font-weight: 600; color: #333; }
.danmaku-header-right { display: flex; align-items: center; gap: 6px; }
.danmaku-count { font-size: 12px; color: #999; }
.danmaku-list { max-height: 280px; overflow-y: auto; transition: max-height 0.25s ease; }
.danmaku-list::-webkit-scrollbar { width: 3px; }
.danmaku-list::-webkit-scrollbar-thumb { background: #ddd; border-radius: 2px; }
.danmaku-item { padding: 4px 0; font-size: 12px; line-height: 1.6; display: flex; gap: 6px; }
.dm-time { color: #999; flex-shrink: 0; font-family: monospace; }
.dm-user { color: #00a1d6; flex-shrink: 0; max-width: 60px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.dm-text { color: #333; word-break: break-all; flex: 1; }
.dm-empty { text-align: center; color: #bbb; padding: 20px 0; font-size: 13px; }

.recommend-section { background: #fff; border-radius: 4px; box-shadow: 0 1px 2px rgba(0,0,0,0.06); padding: 12px 14px; }
.rec-title { font-size: 14px; font-weight: 600; color: #333; margin-bottom: 10px; padding-bottom: 8px; border-bottom: 1px solid #f0f0f0; }
.rec-list { display: flex; flex-direction: column; gap: 10px; }
.rec-card { display: flex; gap: 10px; text-decoration: none; padding: 6px; border-radius: 4px; transition: background 0.2s; }
.rec-card:hover { background: #f8f8f8; }
.rec-card-pic { position: relative; width: 120px; flex-shrink: 0; border-radius: 4px; overflow: hidden; }
.rec-card-pic img { width: 100%; display: block; aspect-ratio: 16 / 9; object-fit: cover; }
.rec-duration { position: absolute; right: 4px; bottom: 4px; background: rgba(0,0,0,0.7); color: #fff; font-size: 11px; padding: 1px 5px; border-radius: 2px; }
.rec-card-info { flex: 1; min-width: 0; display: flex; flex-direction: column; justify-content: space-between; }
.rec-card-title { font-size: 13px; color: #222; line-height: 1.4; margin: 0 0 4px; overflow: hidden; text-overflow: ellipsis; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; }
.rec-card-meta { display: flex; flex-direction: column; gap: 2px; }
.rec-card-author { font-size: 11px; color: #999; }
.rec-card-views { font-size: 11px; color: #999; }

@media (max-width: 1100px) {
  .up-panel-container .up-avatar-img { width: 44px; height: 44px; }
  .up-panel-container .up-description { -webkit-line-clamp: 2; }
}

@media (max-width: 900px) {
  .player-container { flex-direction: column; }
  .player-right { width: 100%; position: static; max-height: none; }
  .up-panel-container .up-info-container { flex-direction: row; text-align: left; }
  .up-panel-container .up-detail-top { justify-content: flex-start; }
  .up-panel-container .up-description { -webkit-line-clamp: 2; }
  .up-panel-container .upinfo-btn-panel { justify-content: flex-start; }
  .mini-player { width: 240px; bottom: 10px; right: 10px; }
  .upnext-inner { flex-direction: column; align-items: center; padding: 16px; }
  .upnext-side { width: 100%; }
  .upnext-card--secondary { flex-direction: row; }
  .upnext-card--secondary .upnext-card-thumb { width: 110px; }
}

@media (max-width: 768px) {
  .player-page { padding-top: 60px; }
  .player-container { padding: 0 10px 20px; }
  .player-stage { border-radius: 0; }
  .player-stage video { aspect-ratio: unset; min-height: 220px; }
  .video-title { font-size: 16px; }
  .video-meta { padding: 12px 14px; }
  .stat-row { gap: 14px; font-size: 13px; }
  .resolution-bar { padding: 6px 12px; overflow-x: auto; }
  .fullscreen-controls { padding: 6px 12px; }
}

/* Fullscreen overrides */
.plyr--fullscreen .player-stage { border-radius: 0; }
.plyr--fullscreen video { aspect-ratio: unset; }
:fullscreen .player-stage { border-radius: 0; background: #000; }
:-webkit-full-screen .player-stage { border-radius: 0; background: #000; }
:-moz-full-screen .player-stage { border-radius: 0; background: #000; }
.plyr.plyr--fullscreen { background: #000; }

.comment-toolbar { display: flex; align-items: center; }
.comment-form-right { display: flex; align-items: center; }
.comment-image-previews { display: flex; gap: 8px; flex-wrap: wrap; }
.comment-image-preview { position: relative; width: 60px; height: 60px; border-radius: 4px; overflow: hidden; }
.comment-image-preview img { width: 100%; height: 100%; object-fit: cover; }
.comment-image-preview .btn-close { position: absolute; top: 2px; right: 2px; width: 16px; height: 16px; font-size: 10px; background: rgba(0,0,0,0.5); border-radius: 50%; padding: 0; }
.comment-images, .reply-images { display: flex; gap: 6px; flex-wrap: wrap; }
.comment-image { width: 120px; height: 80px; object-fit: cover; border-radius: 4px; cursor: pointer; transition: transform 0.2s; }
.comment-image:hover { transform: scale(1.05); }
.video-link { color: #00a1d6; text-decoration: none; font-weight: 500; }
.video-link:hover { text-decoration: underline; }
.video-link-input { background: #fff; }

/* Up Next Overlay - Frosted Glass */
.upnext-fade-enter-active, .upnext-fade-leave-active { transition: opacity 0.35s ease; }
.upnext-fade-enter-from, .upnext-fade-leave-to { opacity: 0; }

.upnext-overlay {
  position: absolute; inset: 0; z-index: 100;
  background: rgba(0, 0, 0, 0.45);
  backdrop-filter: blur(16px) saturate(1.2);
  -webkit-backdrop-filter: blur(16px) saturate(1.2);
  display: flex; flex-direction: column;
  align-items: center; justify-content: center;
  border-radius: 4px; overflow: hidden;
}

.upnext-inner {
  display: flex; align-items: flex-start; gap: 16px;
  padding: 20px 24px 0; width: 100%; max-width: 800px;
  box-sizing: border-box;
}

.upnext-card {
  background: rgba(255, 255, 255, 0.08);
  backdrop-filter: blur(8px);
  -webkit-backdrop-filter: blur(8px);
  border-radius: 8px; overflow: hidden; cursor: pointer;
  transition: all 0.25s ease; border: 1px solid rgba(255,255,255,0.08);
}

.upnext-card:hover {
  background: rgba(255, 255, 255, 0.14);
  transform: translateY(-2px);
  border-color: rgba(255,255,255,0.15);
}

.upnext-card--primary {
  flex: 1; min-width: 0;
  border: 1px solid rgba(251, 114, 153, 0.4);
}

.upnext-card--primary:hover {
  border-color: rgba(251, 114, 153, 0.7);
}

.upnext-badge {
  display: inline-block;
  background: #fb7299; color: #fff;
  font-size: 11px; padding: 3px 10px; border-radius: 4px;
  font-weight: 500; margin: 10px 12px 0;
}

.upnext-countdown-ring {
  display: flex; align-items: center; justify-content: center;
  padding: 14px 0 6px;
  position: relative;
}

.upnext-countdown-ring .countdown-svg {
  width: 64px; height: 64px; transform: rotate(-90deg);
}

.upnext-countdown-ring .countdown-track {
  fill: none; stroke: rgba(255,255,255,0.12); stroke-width: 4;
}

.upnext-countdown-ring .countdown-progress {
  fill: none; stroke: #fb7299; stroke-width: 4;
  stroke-linecap: round;
  stroke-dasharray: 276.46;
  transition: stroke-dashoffset 0.8s linear;
}

.upnext-countdown-ring .countdown-center {
  position: absolute;
}

.upnext-countdown-ring .countdown-number {
  font-size: 22px; font-weight: 700; color: #fff;
  font-variant-numeric: tabular-nums;
}

.upnext-card-thumb {
  position: relative; width: 100%; aspect-ratio: 16 / 9; overflow: hidden;
}

.upnext-card-thumb img {
  width: 100%; height: 100%; object-fit: cover;
  transition: transform 0.3s ease;
}

.upnext-card:hover .upnext-card-thumb img {
  transform: scale(1.05);
}

.upnext-card-duration {
  position: absolute; right: 6px; bottom: 6px;
  background: rgba(0,0,0,0.75); color: #fff;
  font-size: 11px; padding: 1px 6px; border-radius: 3px;
}

.upnext-card-play {
  position: absolute; inset: 0;
  display: flex; align-items: center; justify-content: center;
  background: rgba(0,0,0,0.25); opacity: 0;
  transition: opacity 0.2s ease;
}

.upnext-card:hover .upnext-card-play { opacity: 1; }

.upnext-card-play i {
  color: #fff; font-size: 22px;
  width: 38px; height: 38px; border-radius: 50%;
  background: rgba(251,114,153,0.85);
  display: flex; align-items: center; justify-content: center;
}

.upnext-card-info { padding: 10px 12px; }

.upnext-card-title {
  font-size: 13px; color: #fff; margin: 0 0 6px;
  overflow: hidden; text-overflow: ellipsis;
  display: -webkit-box; -webkit-line-clamp: 2;
  -webkit-box-orient: vertical; line-height: 1.4;
}

.upnext-card-meta {
  display: flex; flex-direction: column; gap: 2px;
  font-size: 11px; color: rgba(255,255,255,0.45);
}

.upnext-side {
  width: 240px; flex-shrink: 0;
  display: flex; flex-direction: column; gap: 10px;
}

.upnext-side-label {
  font-size: 12px; color: rgba(255,255,255,0.4);
  font-weight: 500; letter-spacing: 1px;
  padding: 0 2px;
}

.upnext-card--secondary {
  flex-direction: row;
}

.upnext-card--secondary .upnext-card-thumb {
  width: 110px; flex-shrink: 0; aspect-ratio: 16 / 9;
}

.upnext-card--secondary .upnext-card-info {
  flex: 1; display: flex; flex-direction: column; justify-content: space-between;
}

.upnext-footer {
  padding: 16px 0 20px;
  display: flex; justify-content: center;
}

.upnext-btn {
  padding: 7px 22px; border-radius: 20px; border: 1px solid rgba(255,255,255,0.2);
  background: rgba(255,255,255,0.08); color: rgba(255,255,255,0.7);
  font-size: 13px; cursor: pointer; transition: all 0.2s ease;
  display: flex; align-items: center; gap: 6px;
}

.upnext-btn:hover {
  background: rgba(255,255,255,0.18); color: #fff;
}

.photo-viewer-overlay {
  position: fixed; inset: 0; z-index: 99999; background: rgba(0,0,0,0.85);
  display: flex; align-items: center; justify-content: center; cursor: zoom-out;
}
.photo-viewer-close {
  position: absolute; top: 16px; right: 24px; background: none; border: none;
  color: #fff; font-size: 36px; cursor: pointer; z-index: 1; line-height: 1;
}
.photo-viewer-image {
  max-width: 90vw; max-height: 90vh; object-fit: contain; border-radius: 4px;
  box-shadow: 0 4px 30px rgba(0,0,0,0.5); cursor: default;
  transition: transform 0.15s ease;
}
.photo-zoom-bar {
  position: fixed; bottom: 32px; left: 50%; transform: translateX(-50%);
  display: flex; align-items: center; gap: 8px; background: rgba(0,0,0,0.6);
  padding: 8px 14px; border-radius: 8px; z-index: 1;
}
.photo-zoom-bar button {
  background: none; border: 1px solid rgba(255,255,255,0.3); color: #fff;
  border-radius: 4px; padding: 4px 10px; cursor: pointer; font-size: 13px;
  transition: background 0.2s;
}
.photo-zoom-bar button:hover { background: rgba(255,255,255,0.15); }
.photo-zoom-bar button:disabled { opacity: 0.3; cursor: not-allowed; }
.photo-zoom-level { color: #fff; font-size: 13px; min-width: 44px; text-align: center; }
</style>
