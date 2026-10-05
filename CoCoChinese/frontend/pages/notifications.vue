<template>
  <div class="notifications-page">
    <div class="notifications-header">
      <h1><i class="fa fa-bell"></i> 通知中心</h1>
      <FluentButton v-if="hasUnread" variant="secondary" icon="fa fa-check" @click="markAllRead">
        全部已读
      </FluentButton>
    </div>

    <div v-if="notifications.length" class="notifications-list">
      <div
        v-for="n in notifications"
        :key="n.id"
        :class="['notification-item fluent-card', { unread: !n.is_read }]"
        @click="markRead(n)"
      >
        <div class="notification-icon" :class="`notif-${n.type}`">
          <i :class="getIcon(n.type)"></i>
        </div>
        <div class="notification-content">
          <h4>{{ n.title }}</h4>
          <p>{{ n.body }}</p>
          <span class="notification-time">{{ timeAgo(n.created_at) }}</span>
        </div>
        <div v-if="!n.is_read" class="notification-dot"></div>
      </div>
    </div>
    <div v-else class="empty-state">
      <i class="fa fa-bell-slash fa-4x"></i>
      <p>暂无通知</p>
    </div>
  </div>
</template>

<script setup lang="ts">
const { success } = useToast()
const notifications = ref<any[]>([])
const hasUnread = computed(() => notifications.value.some(n => !n.is_read))

onMounted(async () => {
  try {
    const res: any = await $fetch('/api/notifications')
    notifications.value = res.notifications
  } catch {}
})

function getIcon(type: string) {
  const icons: Record<string, string> = {
    like: 'fa fa-thumbs-up',
    comment: 'fa fa-comment',
    follow: 'fa fa-user-plus',
    system: 'fa fa-bullhorn',
    mention: 'fa fa-at',
    reply: 'fa fa-reply',
  }
  return icons[type] || 'fa fa-bell'
}

async function markRead(n: any) {
  if (n.is_read) return
  try {
    await $fetch('/api/notifications/read', {
      method: 'POST',
      body: { id: n.id },
    })
    n.is_read = true
  } catch {}
}

async function markAllRead() {
  try {
    await $fetch('/api/notifications/read', { method: 'POST', body: {} })
    notifications.value.forEach(n => { n.is_read = true })
    success('已全部标为已读')
  } catch {}
}

function timeAgo(date: string) {
  const diff = Date.now() - new Date(date).getTime()
  const mins = Math.floor(diff / 60000)
  if (mins < 1) return '刚刚'
  if (mins < 60) return `${mins}分钟前`
  const hours = Math.floor(mins / 60)
  if (hours < 24) return `${hours}小时前`
  const days = Math.floor(hours / 24)
  return `${days}天前`
}
</script>

<style scoped>
.notifications-page { max-width: 700px; margin: 0 auto; padding: var(--fluent-spacing-2xl) var(--fluent-spacing-xl); }
.notifications-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: var(--fluent-spacing-2xl); }
.notifications-header h1 { margin: 0; font-size: var(--fluent-font-size-title-large); }
.notifications-header h1 i { color: var(--fluent-accent); margin-right: var(--fluent-spacing-sm); }
.notifications-list { display: flex; flex-direction: column; gap: var(--fluent-spacing-sm); }
.notification-item { display: flex; align-items: flex-start; gap: var(--fluent-spacing-md); padding: var(--fluent-spacing-lg); cursor: pointer; transition: border-color var(--fluent-animation-duration) var(--fluent-animation-easing); }
.notification-item.unread { border-left: 3px solid var(--fluent-accent); }
.notification-icon { width: 40px; height: 40px; display: flex; align-items: center; justify-content: center; border-radius: var(--fluent-radius-circular); flex-shrink: 0; font-size: 16px; }
.notif-like { background: rgba(0, 120, 212, 0.1); color: #0078d4; }
.notif-comment { background: rgba(16, 124, 16, 0.1); color: #107c10; }
.notif-follow { background: rgba(255, 140, 0, 0.1); color: #ff8c00; }
.notif-system { background: rgba(209, 52, 56, 0.1); color: #d13438; }
.notification-content { flex: 1; }
.notification-content h4 { margin: 0 0 4px; font-size: var(--fluent-font-size-body); }
.notification-content p { margin: 0; color: var(--fluent-text-secondary); font-size: var(--fluent-font-size-body); }
.notification-time { font-size: var(--fluent-font-size-caption); color: var(--fluent-text-tertiary); }
.notification-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--fluent-accent); flex-shrink: 0; margin-top: 8px; }
.empty-state { text-align: center; padding: var(--fluent-spacing-4xl) 0; color: var(--fluent-text-secondary); }
.empty-state i { margin-bottom: var(--fluent-spacing-lg); }
</style>
