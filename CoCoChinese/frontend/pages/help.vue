<template>
  <div class="help-page">
    <div class="help-hero">
      <h1><i class="fa fa-question-circle"></i> 帮助中心</h1>
      <p>在这里找到你需要的答案</p>
      <div class="help-search">
        <input
          v-model="searchQuery"
          class="fluent-input help-search-input"
          placeholder="搜索帮助文档..."
          @input="filterSections"
        />
      </div>
    </div>

    <div class="help-content">
      <div v-for="section in filteredSections" :key="section.title" class="help-section fluent-card">
        <h2><i :class="'fa ' + section.icon"></i> {{ section.title }}</h2>
        <div class="help-items">
          <div
            v-for="item in section.items"
            :key="item.q"
            class="help-item"
            :class="{ expanded: expandedItem === item.q }"
            @click="toggleItem(item.q)"
          >
            <div class="help-item-header">
              <span>{{ item.q }}</span>
              <i class="fa" :class="expandedItem === item.q ? 'fa-chevron-up' : 'fa-chevron-down'"></i>
            </div>
            <div v-if="expandedItem === item.q" class="help-item-body">
              <p v-html="item.a"></p>
            </div>
          </div>
        </div>
      </div>

      <div class="help-section fluent-card">
        <h2><i class="fa fa-envelope"></i> 联系我们</h2>
        <p>如果以上内容无法解决你的问题，请通过以下方式联系我们：</p>
        <div class="contact-options">
          <div class="contact-item">
            <i class="fa fa-comments"></i>
            <span>论坛反馈</span>
            <NuxtLink to="/forum" class="fluent-button fluent-button-secondary">前往论坛</NuxtLink>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
useHead({ title: '帮助中心 - Cocokalo' })

const searchQuery = ref('')
const expandedItem = ref('')

const sections = ref([
  {
    title: '账号相关',
    icon: 'fa-user',
    items: [
      { q: '如何注册账号？', a: '点击页面右上角的「注册」按钮，填写用户名、邮箱和密码即可完成注册。注册后即可浏览视频、发表评论和投稿。' },
      { q: '忘记密码怎么办？', a: '在登录页面点击「忘记密码」，输入注册时使用的邮箱地址，系统将发送密码重置链接到你的邮箱。' },
      { q: '如何修改个人资料？', a: '登录后进入「用户中心」，点击「设置」即可修改头像、昵称、个人简介等信息。' },
      { q: '账号被封禁了怎么办？', a: '如果你认为封禁有误，可以通过论坛「站务反馈」板块提交申诉，请提供相关说明和证据。' },
    ],
  },
  {
    title: '视频相关',
    icon: 'fa-film',
    items: [
      { q: '如何上传视频？', a: '登录后点击「投稿」按钮，选择视频文件并填写标题、描述、分类等信息。上传完成后视频将进入审核队列。' },
      { q: '支持哪些视频格式？', a: '目前支持 MP4、AVI、MOV、MKV、FLV 等常见格式。推荐使用 MP4 (H.264) 以获得最佳兼容性。' },
      { q: '视频审核需要多久？', a: '通常在 24 小时内完成审核。如开启 AI 自动审核，大部分视频会在几分钟内通过。' },
      { q: '为什么我的视频播放不了？', a: '请检查：1) 视频格式是否支持 2) 网络连接是否正常 3) 尝试刷新页面。如果问题持续，请在论坛反馈。' },
      { q: '如何创建视频合集？', a: '进入「创作中心」，点击「新建合集」，为合集命名后即可将视频添加到合集中。' },
    ],
  },
  {
    title: '评论与互动',
    icon: 'fa-comments',
    items: [
      { q: '如何发表评论？', a: '在视频播放页面下方找到评论区，登录后即可输入评论内容并发送。支持回复其他用户的评论。' },
      { q: '什么是弹幕？如何发送？', a: '弹幕是在视频播放时滚动显示的实时评论。在播放器中输入弹幕内容并点击发送即可。' },
      { q: '评论被删除了怎么办？', a: '如果评论违反了社区规范（如包含不当内容），会被管理员或 AI 审核系统删除。请遵守社区规则。' },
      { q: '如何举报不当内容？', a: '在视频、评论或弹幕上找到举报按钮，选择举报原因后提交。管理员会尽快处理。' },
    ],
  },
  {
    title: '功能说明',
    icon: 'fa-puzzle-piece',
    items: [
      { q: '什么是 VIP 会员？', a: 'VIP 会员享受无广告、高清画质、专属标识等特权。详情请访问 VIP 页面。' },
      { q: '如何收藏视频？', a: '在视频页面点击收藏按钮，可以选择添加到已有收藏夹或创建新的收藏夹。' },
      { q: '什么是动态？', a: '动态是用户的活动时间线，包括发布视频、评论、关注等操作都会显示在动态中。' },
      { q: '如何关注其他用户？', a: '访问其他用户的个人主页，点击「关注」按钮即可。关注后可在动态中看到对方的更新。' },
    ],
  },
  {
    title: '技术与隐私',
    icon: 'fa-shield',
    items: [
      { q: 'Cocokalo 如何保护我的隐私？', a: '我们严格遵守隐私政策，不会将你的个人信息分享给第三方。详细条款请查看隐私政策页面。' },
      { q: 'AI 审核如何工作？', a: '我们使用 AI 大模型自动审核评论、弹幕和投稿内容，确保社区环境健康。AI 审核仅检测违规内容，不会读取私人信息。' },
      { q: '遇到 Bug 怎么办？', a: '请在论坛「技术交流」板块发帖描述问题，包括使用的浏览器、设备和复现步骤，我们会尽快修复。' },
    ],
  },
])

const filteredSections = computed(() => {
  if (!searchQuery.value.trim()) return sections.value
  const q = searchQuery.value.toLowerCase()
  return sections.value
    .map(s => ({
      ...s,
      items: s.items.filter(
        item => item.q.toLowerCase().includes(q) || item.a.toLowerCase().includes(q)
      ),
    }))
    .filter(s => s.items.length > 0)
})

function toggleItem(q: string) {
  expandedItem.value = expandedItem.value === q ? '' : q
}

function filterSections() {
  expandedItem.value = ''
}
</script>

<style scoped>
.help-page { max-width: 800px; margin: 0 auto; padding: 0 var(--fluent-spacing-xl) var(--fluent-spacing-4xl); }
.help-hero { text-align: center; padding: var(--fluent-spacing-4xl) 0 var(--fluent-spacing-2xl); }
.help-hero h1 { font-size: 28px; margin: 0 0 8px; }
.help-hero h1 i { color: var(--fluent-accent); }
.help-hero p { color: var(--fluent-text-secondary); margin: 0 0 var(--fluent-spacing-xl); }
.help-search-input { max-width: 400px; text-align: center; }
.help-content { display: flex; flex-direction: column; gap: var(--fluent-spacing-xl); }
.help-section { padding: var(--fluent-spacing-xl); }
.help-section h2 { margin: 0 0 var(--fluent-spacing-lg); font-size: 18px; }
.help-section h2 i { color: var(--fluent-accent); margin-right: 8px; }
.help-section > p { color: var(--fluent-text-secondary); margin-bottom: var(--fluent-spacing-lg); }
.help-items { display: flex; flex-direction: column; gap: 1px; background: #f0f0f0; border-radius: 8px; overflow: hidden; }
.help-item { background: #fff; cursor: pointer; }
.help-item-header { display: flex; justify-content: space-between; align-items: center; padding: 14px 16px; font-weight: 500; transition: background 0.15s; }
.help-item-header:hover { background: #f8f9fa; }
.help-item-header i { color: #999; font-size: 12px; }
.help-item-body { padding: 0 16px 14px; color: var(--fluent-text-secondary); font-size: 14px; line-height: 1.7; }
.help-item-body p { margin: 0; }
.help-item.expanded .help-item-header { color: var(--fluent-accent); }
.contact-options { display: flex; gap: var(--fluent-spacing-lg); }
.contact-item { display: flex; align-items: center; gap: var(--fluent-spacing-md); padding: var(--fluent-spacing-lg); background: #f8f9fa; border-radius: 8px; }
.contact-item i { font-size: 20px; color: var(--fluent-accent); }
</style>
