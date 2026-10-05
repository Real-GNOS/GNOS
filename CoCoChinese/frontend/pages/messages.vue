<template>
  <div class="messages-page">
    <div class="messages-container">
      <div class="sidebar" :class="{ 'd-none': tab === 'dm' && activeConversation && isMobile }">
        <div class="sidebar-header">
          <h3>消息中心</h3>
          <button v-if="showMarkAllRead" class="btn-all-read" @click="markAllRead">全部已读</button>
        </div>

        <div class="sidebar-tabs">
          <button v-for="t in tabs" :key="t.key" :class="{ active: tab === t.key }" @click="switchTab(t.key)">
            {{ t.label }}
            <span v-if="t.key === 'unread' && unreadCount" class="badge">{{ unreadCount > 99 ? '99+' : unreadCount }}</span>
          </button>
        </div>

        <div class="sidebar-body">
          <div v-if="loading" class="loading-state">
            <i class="fa fa-circle-o-notch fa-spin fa-2x text-muted"></i>
            <p class="text-muted">加载中...</p>
          </div>
          <template v-else-if="tab === 'dm'">
            <div class="conversation-search">
              <i class="fa fa-search"></i>
              <input v-model="searchQuery" type="text" placeholder="搜索联系人..." @input="onSearchInput">
            </div>
            <div class="conversation-list">
              <div v-for="conv in filteredConversations" :key="conv.withUserId"
                   class="conv-item" :class="{ active: activeConversation?.withUserId === conv.withUserId, unread: conv.hasUnread }"
                   @click="openConversation(conv)">
                <img :src="conv.avatar || '/images/authorImg.webp'" class="conv-avatar">
                <div class="conv-info">
                  <div class="conv-top">
                    <span class="conv-name">{{ conv.username }}</span>
                    <span class="conv-time">{{ conv.lastTime }}</span>
                  </div>
                  <div class="conv-bottom">
                    <span class="conv-last text-truncate">{{ conv.lastContent }}</span>
                    <span v-if="conv.hasUnread" class="unread-badge">{{ conv.unreadCount > 99 ? '99+' : conv.unreadCount }}</span>
                  </div>
                </div>
              </div>
              <div v-if="!filteredConversations.length" class="empty-state">
                <i class="fa fa-inbox fa-3x text-muted mb-2"></i>
                <p class="text-muted">暂无私信</p>
              </div>
            </div>
          </template>

          <div v-else class="message-list">
            <div v-for="msg in filteredMessages" :key="msg.id" class="msg-item" :class="{ unread: !msg.is_read }" @click="clickSystemMsg(msg)">
              <div class="msg-icon-wrap">
                <i v-if="msg.message_type === 'system'" class="fa fa-bell text-warning"></i>
                <i v-else-if="msg.message_type === 'reply'" class="fa fa-reply text-primary"></i>
                <img v-else :src="msg.from_avatar || '/images/authorImg.webp'" class="msg-avatar">
              </div>
              <div class="msg-content">
                <div class="msg-top">
                  <strong>{{ msg.message_type === 'system' ? '系统通知' : (msg.from_display_name || msg.from_username) }}</strong>
                  <small class="text-muted">{{ formatTime(msg.created_at) }}</small>
                </div>
                <p class="msg-text">{{ msg.content }}</p>
                <button v-if="!msg.is_read" class="mark-read-btn" @click.stop="markRead(msg.id)">标记已读</button>
              </div>
            </div>
            <div v-if="!filteredMessages.length" class="empty-state">
              <i class="fa fa-inbox fa-3x text-muted mb-2"></i>
              <p class="text-muted">暂无消息</p>
            </div>
          </div>
        </div>
      </div>

      <div class="main-panel">
        <template v-if="tab !== 'dm'">
          <div class="panel-placeholder">
            <i class="fa fa-envelope fa-4x text-muted mb-3" style="opacity:0.3"></i>
            <p class="text-muted">选择一个标签查看消息</p>
          </div>
        </template>

        <template v-else-if="!activeConversation">
          <div class="panel-placeholder">
            <i class="fa fa-comments fa-4x text-muted mb-3" style="opacity:0.3"></i>
            <p class="text-muted">选择一个对话开始聊天</p>
          </div>
        </template>

        <template v-else>
          <div class="chat-view">
            <div class="chat-header">
              <button v-if="isMobile" class="btn-back" @click="closeConversation">&larr;</button>
              <img :src="activeConvUser.avatar || '/images/authorImg.webp'" class="chat-avatar">
              <div class="chat-user-info">
                <span class="chat-username">{{ activeConvUser.name }}</span>
              </div>
            </div>

            <div class="chat-messages" ref="chatRef" @scroll="onChatScroll">
              <div v-if="loadingMore" class="load-more text-center py-2">
                <span class="text-muted small">加载中...</span>
              </div>
              <div v-for="msg in chatMessages" :key="msg.id" class="msg-row" :class="{ mine: msg.isMine, yours: !msg.isMine }">
                <div class="msg-bubble-wrap">
                  <div class="msg-bubble" :class="msg.isMine ? 'mine' : 'yours'">
                    <div v-if="msg.image_url" class="msg-image">

                        <img :src="msg.image_url" @click="previewImage(msg.image_url)" @load="scrollChat">

                    </div>
                    <div v-if="msg.content" class="msg-text">{{ msg.content }}</div>
                  </div>
                  <div class="msg-meta">
                    <span class="msg-time">{{ msg.time }}</span>
                  </div>
                </div>
              </div>
            </div>

            <div class="chat-input-area">
              <div v-if="showEmojiPicker" class="emoji-picker" ref="emojiRef">
                <div class="emoji-categories">
                  <button v-for="cat in emojiCategories" :key="cat.name" :class="{ active: emojiCategory === cat.name }" @click="emojiCategory = cat.name" :title="cat.label">{{ cat.icon }}</button>
                </div>
                <div class="emoji-grid">
                  <button v-for="emoji in currentEmojis" :key="emoji" class="emoji-btn" @click="insertEmoji(emoji)">{{ emoji }}</button>
                </div>
              </div>

              <div class="input-toolbar">
                <button class="toolbar-btn" :class="{ active: showEmojiPicker }" @click="toggleEmojiPicker" title="表情">
                  <svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm0 18c-4.41 0-8-3.59-8-8s3.59-8 8-8 8 3.59 8 8-3.59 8-8 8zm3.5-9c.83 0 1.5-.67 1.5-1.5S16.33 8 15.5 8 14 8.67 14 9.5s.67 1.5 1.5 1.5zm-7 0c.83 0 1.5-.67 1.5-1.5S9.33 8 8.5 8 7 8.67 7 9.5 7.67 11 8.5 11zm3.5 6.5c2.33 0 4.31-1.46 5.11-3.5H6.89c.8 2.04 2.78 3.5 5.11 3.5z"/></svg>
                </button>
                <button class="toolbar-btn" @click="triggerImageUpload" title="发送图片">
                  <svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor"><path d="M21 19V5c0-1.1-.9-2-2-2H5c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h14c1.1 0 2-.9 2-2zM8.5 13.5l2.5 3.01L14.5 12l4.5 6H5l3.5-4.5z"/></svg>
                </button>
                <input ref="fileInput" type="file" accept="image/jpeg,image/png,image/gif,image/webp" hidden @change="onImageSelected">
              </div>

              <div v-if="imagePreview" class="image-preview-bar">
                <img :src="imagePreview">
                <button class="remove-image" @click="clearImagePreview">&times;</button>
              </div>

              <div class="input-row">
                <input v-model="chatInput" type="text" class="chat-input" placeholder="输入消息..." maxlength="500" @keyup.enter="sendChatMessage">
                <button class="btn-send" @click="sendChatMessage" :disabled="(!chatInput.trim() && !imagePreview) || sendingImage || isBanned">
                  <span v-if="sendingImage">上传中</span>
                  <span v-else>发送</span>
                </button>
              </div>
            </div>
          </div>
        </template>
      </div>
    </div>

    <div v-if="previewUrl" class="image-overlay" @click.self="previewUrl = ''">
      <img :src="previewUrl">
      <button class="close-overlay" @click="previewUrl = ''">&times;</button>
    </div>
  </div>
</template>

<script setup>
const messages = ref([])
const loading = ref(true)
const tab = ref('dm')
const chatMessages = ref([])
const chatInput = ref('')
const activeConversation = ref(null)
const chatRef = ref(null)
const me = ref(null)
const showEmojiPicker = ref(false)
const emojiCategory = ref('smileys')
const fileInput = ref(null)
const imagePreview = ref('')
const sendingImage = ref(false)
const loadingMore = ref(false)
const previewUrl = ref('')
const searchQuery = ref('')
const isMobile = ref(false)
const route = useRoute()
let pollTimer = null

const emojiCategories = [
  { name: 'smileys', icon: '😊', label: '笑脸' },
  { name: 'gestures', icon: '👍', label: '手势' },
  { name: 'hearts', icon: '❤️', label: '爱心' },
  { name: 'animals', icon: '🐱', label: '动物' },
  { name: 'food', icon: '🍎', label: '食物' },
  { name: 'nature', icon: '🌺', label: '自然' },
  { name: 'objects', icon: '🎉', label: '物品' },
  { name: 'symbols', icon: '💯', label: '符号' },
]

const emojiMap = {
  smileys: ['😀','😃','😄','😁','😅','😂','🤣','😊','😇','🙂','😉','😌','😍','🥰','😘','😗','😋','😛','😜','🤪','😝','🤑','🤗','🤭','🤔','🤐','😐','😑','😶','😏','😒','🙄','😬','🤥','😔','😪','🤤','😴','😷','🤒','🤕','🤢','🤮','🥴','😵','🤯','🤠','🥳','🥺','😢','😭','😤','😡','🤬','😈','👿','💀','☠️','💩'],
  gestures: ['👍','👎','👊','✊','🤛','🤜','👏','🙌','👐','🤲','🤝','🙏','✌️','🤟','🤘','👌','👋','🤙','💪','✋','🤚','🖐️'],
  hearts: ['❤️','🧡','💛','💚','💙','💜','🖤','🤍','🤎','💔','❣️','💕','💞','💗','💖','💘','💝','💟'],
  animals: ['🐱','🐶','🐼','🐯','🦁','🐮','🐷','🐸','🐵','🐒','🐔','🐧','🐦','🐤','🐣','🦆','🦅','🦉','🐺','🐴','🦄','🐝','🦋','🐌','🐞','🐢','🐍','🐙','🐬','🐳'],
  food: ['🍎','🍊','🍋','🍌','🍉','🍇','🍓','🍈','🍒','🍑','🥭','🍍','🥥','🥝','🍅','🍆','🥑','🥦','🥬','🥒','🌽','🥕','🥔','🍞','🥖','🧀','🥚','🍳','🥞','🍕','🍔','🍟','🌭'],
  nature: ['🌺','🌸','🌹','🌻','🌷','🌿','🍀','🌵','🌲','🌳','🌴','☀️','🌈','⭐','🌟','✨','⚡','☁️','🌊','🔥'],
  objects: ['🎉','🎊','🎈','🎁','🏆','🚀','📱','💻','⌚️','📸','🔔','📨','💌','📝','✏️','🔑','💡','💰','👑','🎵','🎶','🎮'],
  symbols: ['💯','✅','❌','❓','❗','🚫','🔞','♻️','💠','🔴','🟠','🟡','🟢','🔵','🟣','⚫','⚪','🟤'],
}

const currentEmojis = computed(() => emojiMap[emojiCategory.value] || emojiMap.smileys)

const tabs = [
  { key: 'all', label: '全部' },
  { key: 'unread', label: '未读' },
  { key: 'reply', label: '回复' },
  { key: 'system', label: '系统' },
  { key: 'dm', label: '私信' },
]

const showMarkAllRead = computed(() => tab.value !== 'dm' || !activeConversation.value)

const unreadCount = computed(() => messages.value.filter(m => !m.is_read).length)

const filteredMessages = computed(() => {
  if (tab.value === 'unread') return messages.value.filter(m => !m.is_read)
  if (tab.value === 'all') return messages.value
  return messages.value.filter(m => m.message_type === tab.value)
})

function timeAgo(dateStr) {
  if (!dateStr) return ''
  const now = Date.now()
  const t = new Date(dateStr).getTime()
  const diff = now - t
  const mins = Math.floor(diff / 60000)
  if (mins < 1) return '刚刚'
  if (mins < 60) return `${mins}分钟前`
  const hours = Math.floor(mins / 60)
  if (hours < 24) return `${hours}小时前`
  const days = Math.floor(hours / 24)
  if (days < 7) return `${days}天前`
  return new Date(dateStr).toLocaleDateString('zh-CN', { month: 'numeric', day: 'numeric' })
}

const conversations = computed(() => {
  const dmMessages = messages.value.filter(m => m.message_type === 'dm')
  const groups = new Map()
  for (const msg of dmMessages) {
    const isMine = msg.from_user_id === me.value?.userId
    const otherUserId = isMine ? msg.to_user_id : msg.from_user_id
    const otherName = isMine ? (msg.to_display_name || msg.to_username) : (msg.from_display_name || msg.from_username)
    const otherAvatar = isMine ? msg.to_avatar : msg.from_avatar
    if (!groups.has(otherUserId)) {
      groups.set(otherUserId, { withUserId: otherUserId, username: otherName, avatar: otherAvatar, messages: [], hasUnread: false, unreadCount: 0 })
    }
    const g = groups.get(otherUserId)
    g.messages.push(msg)
    if (!msg.is_read && msg.to_user_id === me.value?.userId) {
      g.hasUnread = true
      g.unreadCount++
    }
  }
  return [...groups.values()].map(g => {
    const last = g.messages[g.messages.length - 1]
    return {
      ...g,
      lastContent: last.image_url ? '[图片]' : last.content,
      lastTime: timeAgo(last.created_at),
    }
  }).sort((a, b) => new Date(b.messages[b.messages.length - 1].created_at) - new Date(a.messages[a.messages.length - 1].created_at))
})

const filteredConversations = computed(() => {
  if (!searchQuery.value.trim()) return conversations.value
  const q = searchQuery.value.toLowerCase()
  return conversations.value.filter(c => c.username.toLowerCase().includes(q))
})

const activeConvUser = computed(() => {
  if (!activeConversation.value) return {}
  return { name: activeConversation.value.username, avatar: activeConversation.value.avatar }
})

function startPolling() {
  stopPolling()
  pollTimer = setInterval(fetchChatMessages, 3000)
}

function stopPolling() {
  if (pollTimer) {
    clearInterval(pollTimer)
    pollTimer = null
  }
}

async function fetchChatMessages() {
  if (!activeConversation.value) return
  try {
    const res = await $fetch('/api/messages', { params: { type: 'dm' } })
    if (!res.success) return
    const conv = activeConversation.value
    const relevant = res.data.filter(m =>
      (m.from_user_id === conv.withUserId && m.to_user_id === me.value?.userId) ||
      (m.to_user_id === conv.withUserId && m.from_user_id === me.value?.userId)
    )
    const existingIds = new Set(chatMessages.value.map(m => m.id))
    const newOnes = relevant
      .filter(m => !existingIds.has(m.id))
      .map(m => ({
        id: m.id,
        content: m.content,
        image_url: m.image_url || '',
        time: timeAgo(m.created_at),
        isMine: m.from_user_id === me.value?.userId,
      }))
    if (newOnes.length) {
      chatMessages.value.push(...newOnes)
      nextTick(() => scrollChat())
    }
  } catch {}
}

function onSearchInput() {}

function toggleEmojiPicker() {
  showEmojiPicker.value = !showEmojiPicker.value
}

function insertEmoji(emoji) {
  chatInput.value += emoji
  chatInput.value = chatInput.value
  showEmojiPicker.value = false
}

function triggerImageUpload() {
  fileInput.value?.click()
}

function onImageSelected(e) {
  const file = e.target?.files?.[0]
  if (!file) return
  if (file.size > 10 * 1024 * 1024) {
    alert('图片大小不能超过10MB')
    return
  }
  const reader = new FileReader()
  reader.onload = (ev) => {
    imagePreview.value = ev.target?.result || ''
  }
  reader.readAsDataURL(file)
  e.target.value = ''
}

function clearImagePreview() {
  imagePreview.value = ''
}

async function sendChatMessage() {
  const text = chatInput.value.trim()
  if ((!text && !imagePreview.value) || !activeConversation.value) return
  sendingImage.value = true

  let imageUrl = ''
  if (imagePreview.value) {
    try {
      const blob = await (await fetch(imagePreview.value)).blob()
      const formData = new FormData()
      formData.append('file', blob, 'chat_image.png')
      const uploadRes = await $fetch('/api/upload/chat-image', { method: 'POST', body: formData })
      imageUrl = uploadRes.url
    } catch {
      alert('图片上传失败')
      sendingImage.value = false
      return
    }
  }

  try {
    const res = await $fetch('/api/messages', {
      method: 'POST',
      body: {
        to_user_id: activeConversation.value.withUserId,
        content: text,
        imageUrl,
        message_type: 'dm',
      },
    })
    if (res.success && res.data) {
      chatMessages.value.push({
        id: res.data.id,
        content: res.data.content,
        image_url: res.data.image_url || '',
        time: '刚刚',
        isMine: true,
      })
      nextTick(() => scrollChat())
    }
  } catch {
    alert('发送失败')
  }

  chatInput.value = ''
  imagePreview.value = ''
  sendingImage.value = false
}

function scrollChat() {
  nextTick(() => {
    if (chatRef.value) chatRef.value.scrollTop = chatRef.value.scrollHeight
  })
}

function onChatScroll() {
  if (chatRef.value && chatRef.value.scrollTop < 50 && !loadingMore.value) {
    loadingMore.value = true
    setTimeout(() => { loadingMore.value = false }, 1000)
  }
}

async function openConversation(conv) {
  activeConversation.value = conv
  chatMessages.value = conv.messages.map(m => ({
    id: m.id,
    content: m.content,
    image_url: m.image_url || '',
    time: timeAgo(m.created_at),
    isMine: m.from_user_id !== conv.withUserId,
  }))
  chatMessages.value.reverse()
  await markConversationRead(conv)
  startPolling()
  nextTick(() => scrollChat())
  showEmojiPicker.value = false
}

function closeConversation() {
  stopPolling()
  activeConversation.value = null
  chatMessages.value = []
  chatInput.value = ''
  imagePreview.value = ''
  showEmojiPicker.value = false
}

async function switchTab(t) {
  tab.value = t
  closeConversation()
  await fetchMessages()
}

async function fetchMessages() {
  try {
    const type = tab.value === 'all' || tab.value === 'unread' ? 'all' : tab.value
    const res = await $fetch(`/api/messages?type=${type}`)
    if (res.success) messages.value = res.data
  } catch {} finally {
    loading.value = false
  }
}

async function markConversationRead(conv) {
  const ids = conv.messages.filter(m => !m.is_read && m.to_user_id === me.value?.userId).map(m => m.id)
  if (!ids.length) return
  try {
    await $fetch('/api/messages/read', { method: 'POST', body: { message_ids: ids } })
    conv.hasUnread = false
    conv.unreadCount = 0
    messages.value.forEach(m => {
      if (ids.includes(m.id)) m.is_read = true
    })
  } catch {}
}

async function markRead(id) {
  try {
    await $fetch('/api/messages/read', { method: 'POST', body: { message_ids: [id] } })
    const msg = messages.value.find(m => m.id === id)
    if (msg) msg.is_read = true
  } catch {}
}

async function markAllRead() {
  try {
    await $fetch('/api/messages/read', { method: 'POST', body: {} })
    messages.value.forEach(m => m.is_read = true)
  } catch {}
}

function clickSystemMsg(msg) {
  if (msg.message_type === 'dm') {
    const conv = conversations.value.find(c => c.withUserId === (msg.from_user_id === me.value?.userId ? msg.to_user_id : msg.from_user_id))
    if (conv) {
      tab.value = 'dm'
      openConversation(conv)
    }
  }
}

function formatTime(dateStr) {
  if (!dateStr) return ''
  const d = new Date(dateStr)
  const now = new Date()
  const diff = now - d
  if (diff < 86400000) return d.toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit' })
  return d.toLocaleDateString('zh-CN')
}

function previewImage(url) {
  previewUrl.value = url
}

onMounted(async () => {
  isMobile.value = window.innerWidth < 768
  window.addEventListener('resize', () => { isMobile.value = window.innerWidth < 768 })
  try {
    const res = await $fetch('/api/user/me')
    if (res.success && res.user) me.value = res.user
  } catch {}
  await fetchMessages()

  if (route.query.dm) {
    tab.value = 'dm'
    await nextTick()
    const targetId = parseInt(route.query.dm)
    let conv = conversations.value.find(c => c.withUserId === targetId)
    if (!conv) {
      try {
        const res = await $fetch(`/api/user/by-id/${targetId}`)
        if (res.success && res.user) {
          conv = {
            withUserId: targetId,
            username: res.user.display_name || res.user.username,
            avatar: res.user.avatar_url || '/images/authorImg.webp',
            messages: [],
            hasUnread: false,
            unreadCount: 0,
            lastContent: '',
            lastTime: '',
          }
        }
      } catch {}
    }
    if (conv) openConversation(conv)
  }
})

onUnmounted(() => {
  stopPolling()
})
const { banned: isBanned } = useBan()
</script>

<style scoped>
.messages-page {
  min-height: calc(100vh - 60px);
  background: #f4f4f5;
  padding: 16px;
}

.messages-container {
  max-width: 960px;
  margin: 0 auto;
  height: calc(100vh - 100px);
  display: flex;
  background: #fff;
  border-radius: 12px;
  box-shadow: 0 2px 12px rgba(0,0,0,0.08);
  overflow: hidden;
}

.sidebar {
  width: 320px;
  min-width: 320px;
  display: flex;
  flex-direction: column;
  border-right: 1px solid #e8e8e8;
}

.sidebar-header {
  padding: 16px 20px 12px;
  border-bottom: 1px solid #f0f0f0;
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.sidebar-header h3 {
  margin: 0;
  font-size: 18px;
  font-weight: 700;
}

.btn-all-read {
  font-size: 12px;
  color: #999;
  background: none;
  border: none;
  cursor: pointer;
}

.btn-all-read:hover { color: #00a1d6; }

.sidebar-tabs {
  display: flex;
  padding: 8px 12px;
  gap: 4px;
  border-bottom: 1px solid #f0f0f0;
  overflow-x: auto;
}

.sidebar-tabs button {
  flex: 1;
  padding: 6px 10px;
  border: none;
  background: none;
  font-size: 13px;
  color: #666;
  cursor: pointer;
  border-radius: 6px;
  white-space: nowrap;
  position: relative;
  transition: all 0.2s;
}

.sidebar-tabs button:hover { background: #f0f0f0; color: #333; }
.sidebar-tabs button.active { background: #e8f4fe; color: #00a1d6; font-weight: 600; }
.sidebar-tabs button .badge {
  position: absolute;
  top: -2px;
  right: 2px;
  background: #fb7299;
  color: #fff;
  font-size: 10px;
  padding: 1px 5px;
  border-radius: 10px;
  min-width: 16px;
  text-align: center;
}

.sidebar-body {
  flex: 1;
  overflow-y: auto;
}

.conversation-search {
  padding: 8px 12px;
  position: relative;
}

.conversation-search input {
  width: 100%;
  padding: 8px 12px 8px 32px;
  border: 1px solid #e8e8e8;
  border-radius: 20px;
  font-size: 13px;
  outline: none;
  background: #f5f5f5;
  transition: all 0.2s;
}

.conversation-search input:focus {
  border-color: #00a1d6;
  background: #fff;
}

.conversation-search .fa-search {
  position: absolute;
  left: 22px;
  top: 50%;
  transform: translateY(-50%);
  color: #999;
  font-size: 13px;
}

.conversation-list, .message-list { padding: 0; }

.conv-item {
  display: flex;
  padding: 12px 16px;
  gap: 12px;
  cursor: pointer;
  transition: background 0.15s;
  align-items: center;
}

.conv-item:hover { background: #f7f8fa; }
.conv-item.active { background: #e8f4fe; }
.conv-item.unread { background: #fff; }

.conv-avatar {
  width: 48px;
  height: 48px;
  border-radius: 50%;
  object-fit: cover;
  flex-shrink: 0;
}

.conv-info { flex: 1; min-width: 0; }

.conv-top {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 4px;
}

.conv-name {
  font-size: 14px;
  font-weight: 600;
  color: #222;
}

.conv-time {
  font-size: 11px;
  color: #999;
  flex-shrink: 0;
}

.conv-bottom {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.conv-last {
  font-size: 13px;
  color: #999;
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.unread-badge {
  background: #fb7299;
  color: #fff;
  font-size: 10px;
  padding: 1px 6px;
  border-radius: 10px;
  min-width: 18px;
  text-align: center;
  flex-shrink: 0;
  margin-left: 6px;
}

.msg-item {
  display: flex;
  padding: 14px 16px;
  gap: 12px;
  cursor: pointer;
  transition: background 0.15s;
}

.msg-item:hover { background: #f7f8fa; }
.msg-item.unread { background: #fafafa; }

.msg-icon-wrap {
  width: 40px;
  height: 40px;
  display: flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
}

.msg-icon-wrap i { font-size: 22px; }

.msg-avatar {
  width: 40px;
  height: 40px;
  border-radius: 50%;
  object-fit: cover;
}

.msg-content { flex: 1; min-width: 0; }

.msg-top {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 4px;
}

.msg-top strong { font-size: 14px; color: #222; }

.msg-text {
  font-size: 13px;
  color: #666;
  margin: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.mark-read-btn {
  font-size: 11px;
  color: #00a1d6;
  background: none;
  border: none;
  cursor: pointer;
  padding: 0;
  margin-top: 4px;
}

.mark-read-btn:hover { text-decoration: underline; }

.main-panel {
  flex: 1;
  display: flex;
  flex-direction: column;
  min-width: 0;
}

.panel-placeholder {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  color: #999;
}

.chat-view {
  display: flex;
  flex-direction: column;
  height: 100%;
}

.chat-header {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 14px 20px;
  border-bottom: 1px solid #f0f0f0;
  background: #fff;
}

.btn-back {
  background: none;
  border: none;
  font-size: 20px;
  cursor: pointer;
  color: #666;
  padding: 0 4px;
}

.chat-avatar {
  width: 40px;
  height: 40px;
  border-radius: 50%;
  object-fit: cover;
}

.chat-user-info {
  flex: 1;
  display: flex;
  align-items: center;
  gap: 8px;
}

.chat-username {
  font-size: 16px;
  font-weight: 600;
  color: #222;
}

.online-status {
  font-size: 11px;
  color: #52c41a;
  background: #f0fff0;
  padding: 1px 8px;
  border-radius: 10px;
}

.offline-status {
  font-size: 11px;
  color: #999;
}

.typing-badge {
  font-size: 12px;
  color: #00a1d6;
  background: #e8f4fe;
  padding: 2px 10px;
  border-radius: 10px;
  animation: pulse 1.5s ease-in-out infinite;
}

@keyframes pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.5; }
}

.chat-messages {
  flex: 1;
  overflow-y: auto;
  padding: 16px 20px;
  background: #fafafa;
}

.msg-row {
  display: flex;
  margin-bottom: 16px;
}

.msg-row.mine { justify-content: flex-end; }
.msg-row.yours { justify-content: flex-start; }

.msg-bubble-wrap {
  max-width: 65%;
  min-width: 0;
}

.msg-bubble {
  padding: 10px 14px;
  border-radius: 16px;
  word-break: break-word;
  font-size: 14px;
  line-height: 1.5;
  position: relative;
}

.msg-bubble.mine {
  background: #00a1d6;
  color: #fff;
  border-bottom-right-radius: 4px;
}

.msg-bubble.yours {
  background: #fff;
  color: #222;
  border-bottom-left-radius: 4px;
  box-shadow: 0 1px 3px rgba(0,0,0,0.06);
}

.msg-image {
  margin: -6px -10px 4px;
  overflow: hidden;
}

.msg-image img {
  max-width: 100%;
  max-height: 300px;
  border-radius: 12px;
  display: block;
  cursor: pointer;
  transition: transform 0.2s;
}

.msg-image img:hover {
  transform: scale(1.02);
}

.msg-text { margin: 0; }

.msg-meta {
  display: flex;
  justify-content: flex-end;
  margin-top: 2px;
}

.msg-time {
  font-size: 11px;
  color: #bbb;
}

.chat-input-area {
  border-top: 1px solid #f0f0f0;
  background: #fff;
  position: relative;
}

.emoji-picker {
  position: absolute;
  bottom: 100%;
  left: 0;
  right: 0;
  background: #fff;
  border-top: 1px solid #e8e8e8;
  border-radius: 12px 12px 0 0;
  box-shadow: 0 -4px 12px rgba(0,0,0,0.08);
  padding: 8px;
  z-index: 100;
  max-height: 260px;
  display: flex;
  flex-direction: column;
}

.emoji-categories {
  display: flex;
  gap: 4px;
  padding: 4px 0 8px;
  border-bottom: 1px solid #f0f0f0;
  overflow-x: auto;
}

.emoji-categories button {
  background: none;
  border: none;
  font-size: 20px;
  padding: 4px 10px;
  cursor: pointer;
  border-radius: 6px;
  transition: background 0.15s;
}

.emoji-categories button:hover { background: #f0f0f0; }
.emoji-categories button.active { background: #e8f4fe; }

.emoji-grid {
  display: grid;
  grid-template-columns: repeat(8, 1fr);
  gap: 2px;
  padding: 8px 0;
  overflow-y: auto;
  flex: 1;
}

.emoji-btn {
  background: none;
  border: none;
  font-size: 22px;
  padding: 4px;
  cursor: pointer;
  border-radius: 6px;
  text-align: center;
  transition: background 0.1s;
}

.emoji-btn:hover { background: #f0f0f0; }

.input-toolbar {
  display: flex;
  gap: 2px;
  padding: 8px 12px 4px;
}

.toolbar-btn {
  background: none;
  border: none;
  width: 36px;
  height: 36px;
  display: flex;
  align-items: center;
  justify-content: center;
  cursor: pointer;
  border-radius: 8px;
  color: #666;
  transition: all 0.15s;
}

.toolbar-btn:hover { background: #f0f0f0; color: #333; }
.toolbar-btn.active { background: #e8f4fe; color: #00a1d6; }

.image-preview-bar {
  display: flex;
  align-items: center;
  padding: 4px 12px;
  gap: 8px;
}

.image-preview-bar img {
  max-width: 80px;
  max-height: 60px;
  border-radius: 8px;
  object-fit: cover;
}

.remove-image {
  background: rgba(0,0,0,0.5);
  color: #fff;
  border: none;
  width: 20px;
  height: 20px;
  border-radius: 50%;
  cursor: pointer;
  font-size: 14px;
  display: flex;
  align-items: center;
  justify-content: center;
}

.input-row {
  display: flex;
  gap: 8px;
  padding: 4px 12px 12px;
}

.chat-input {
  flex: 1;
  border: none;
  outline: none;
  font-size: 14px;
  padding: 8px 0;
  color: #222;
  background: transparent;
}

.chat-input::placeholder { color: #bbb; }

.btn-send {
  background: #00a1d6;
  color: #fff;
  border: none;
  padding: 8px 18px;
  border-radius: 8px;
  font-size: 14px;
  cursor: pointer;
  transition: background 0.2s;
  white-space: nowrap;
}

.btn-send:hover { background: #0088b3; }
.btn-send:disabled { background: #ccc; cursor: not-allowed; }

.image-overlay {
  position: fixed;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  background: rgba(0,0,0,0.8);
  z-index: 2000;
  display: flex;
  align-items: center;
  justify-content: center;
}

.image-overlay img {
  max-width: 90%;
  max-height: 90%;
  border-radius: 8px;
}

.close-overlay {
  position: fixed;
  top: 20px;
  right: 20px;
  background: rgba(0,0,0,0.5);
  color: #fff;
  border: none;
  width: 36px;
  height: 36px;
  border-radius: 50%;
  font-size: 24px;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
}

.close-overlay:hover { background: rgba(0,0,0,0.7); }

.empty-state {
  text-align: center;
  padding: 60px 20px;
}

.empty-state p { margin: 0; }

.loading-state {
  text-align: center;
  padding: 60px 20px;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 12px;
}
.loading-state p { margin: 0; }

.load-more span {
  display: inline-block;
  padding: 4px 12px;
  background: #f0f0f0;
  border-radius: 10px;
}

@media (max-width: 767px) {
  .messages-page { padding: 0; }
  .messages-container {
    height: calc(100vh - 56px);
    border-radius: 0;
  }
  .sidebar {
    width: 100%;
    min-width: auto;
  }
  .main-panel {
    width: 100%;
  }
  .msg-bubble-wrap { max-width: 80%; }
}
</style>
