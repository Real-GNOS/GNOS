<template>
  <div>
    <Nav :mainNav="mainNav" />
    <div v-if="banned" class="global-ban-banner">
      <i class="fa fa-exclamation-circle"></i>
      <span>
        你的账号已被封禁{{ banInfo?.expires_at ? '至 ' + formatExpire(banInfo.expires_at) : '（永久）' }}，
        原因：{{ banInfo?.reason || '违反社区规定' }}。封禁期间无法发布内容、评论、上传或私信。
      </span>
    </div>
    <div v-if="currentNotice" class="global-notice-banner">
      <i class="fa fa-bullhorn"></i>
      <div class="notice-body">
        <strong class="notice-title">{{ currentNotice.title }}</strong>
        <span class="notice-content">{{ currentNotice.content }}</span>
        <NuxtLink v-if="currentNotice.link" :to="currentNotice.link" class="notice-link">查看详情</NuxtLink>
        <NuxtLink to="/announcements" class="notice-all">查看全部</NuxtLink>
      </div>
      <button class="notice-close" @click="dismissCurrent" aria-label="关闭公告">×</button>
    </div>
    <main class="main-content">
      <slot />
    </main>
    <Footer :footer="footerData" />
  </div>
</template>

<script setup>
const mainNav = ref({ leftList: [], rightList: [], loginFlag: false })
const footerData = ref({ about: [], send: [], imgSend: [] })

const { banned, banInfo, fetchBan, formatExpire } = useBan()

const notices = ref([])
const currentIdx = ref(0)
const dismissed = ref({})
let rotateTimer = null

function loadDismissed() {
  try { dismissed.value = JSON.parse(localStorage.getItem('cocokalo_dismissed_notices') || '{}') } catch (e) { dismissed.value = {} }
}
function saveDismissed() {
  localStorage.setItem('cocokalo_dismissed_notices', JSON.stringify(dismissed.value))
}
const visibleNotices = computed(() => notices.value.filter(n => !dismissed.value[n.id]))
const currentNotice = computed(() => visibleNotices.value[currentIdx.value] || null)

function dismissCurrent() {
  if (currentNotice.value) {
    dismissed.value[currentNotice.value.id] = true
    saveDismissed()
    currentIdx.value = 0
  }
}
function startRotate() {
  stopRotate()
  if (visibleNotices.value.length > 1) {
    rotateTimer = setInterval(() => {
      if (visibleNotices.value.length <= 1) { stopRotate(); return }
      currentIdx.value = (currentIdx.value + 1) % visibleNotices.value.length
    }, 5000)
  }
}
function stopRotate() { if (rotateTimer) { clearInterval(rotateTimer); rotateTimer = null } }

onMounted(async () => {
  fetchBan()
  loadDismissed()
  try {
    const res = await $fetch('/api/notices')
    notices.value = (res && res.notices) || []
    currentIdx.value = 0
    startRotate()
  } catch (e) {}
})
onBeforeUnmount(stopRotate)

try {
  const { data } = await useFetch('/api/home')
  if (data.value) {
    mainNav.value = data.value.mainNav || mainNav.value
    footerData.value = data.value.footer || footerData.value
  }
} catch (e) {
  console.error('Failed to load home data:', e)
}
</script>

<style>
.main-content { margin-top: 60px; min-height: calc(100vh - 200px); }

.global-ban-banner {
  background: #fef0f0;
  color: #c0392b;
  border-bottom: 1px solid #f5c6cb;
  padding: 10px 20px;
  font-size: 14px;
  display: flex;
  align-items: center;
  gap: 8px;
  line-height: 1.5;
}
.global-ban-banner i { font-size: 16px; }

.global-notice-banner {
  background: linear-gradient(90deg, #6c5ce7, #a29bfe);
  color: #fff;
  padding: 8px 20px;
  font-size: 14px;
  display: flex;
  align-items: center;
  gap: 10px;
}
.global-notice-banner i { font-size: 16px; }
.notice-body { display: flex; align-items: center; gap: 10px; flex: 1; min-width: 0; overflow: hidden; }
.notice-title { font-weight: 600; white-space: nowrap; }
.notice-content { opacity: 0.95; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.notice-link, .notice-all { color: #fff; text-decoration: underline; white-space: nowrap; }
.notice-close { background: transparent; border: none; color: #fff; font-size: 20px; cursor: pointer; line-height: 1; padding: 0 4px; }
</style>
