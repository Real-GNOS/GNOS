<template>
  <div class="settings-page">
    <div class="settings-container">
      <h1><i class="fa fa-cog"></i> 账号设置</h1>

      <div class="settings-section fluent-card">
        <h2>个人资料</h2>
        <FluentTextField v-model="displayName" label="显示名称" placeholder="输入显示名称" />
        <FluentTextField v-model="bio" label="个人简介" placeholder="介绍一下自己" />
        <FluentTextField v-model="location" label="所在地" placeholder="城市" />
        <FluentTextField v-model="website" label="个人网站" placeholder="https://" />

        <h3 class="mt-3">个人封面</h3>
        <div class="banner-preview">
          <img :src="bannerUrl || '/images/banner1.jpg'" alt="封面预览" />
        </div>
        <div class="banner-upload">
          <FluentTextField v-model="bannerUrl" label="封面图URL" placeholder="输入封面图片地址" />
          <FluentButton variant="primary" @click="saveBanner">保存封面</FluentButton>
        </div>

        <div class="settings-actions">
          <FluentButton variant="primary" icon="fa fa-save" @click="saveProfile">保存</FluentButton>
        </div>
      </div>

      <div class="settings-section fluent-card">
        <h2>账号安全</h2>
        <FluentTextField v-model="email" label="邮箱" type="email" placeholder="绑定邮箱" />
        <div class="settings-actions">
          <FluentButton variant="primary" @click="saveEmail">保存邮箱</FluentButton>
          <FluentButton variant="secondary" icon="fa fa-send" @click="sendVerification">
            发送验证邮件
          </FluentButton>
        </div>
      </div>

      <div class="settings-section fluent-card">
        <h2>已连接的账号</h2>
        <div class="connected-accounts">
          <div class="account-item">
            <i class="fa fa-google" style="color: #4285f4;"></i>
            <span>Google</span>
            <span class="account-status disconnected">未连接</span>
          </div>
          <div class="account-item">
            <i class="fa fa-discord" style="color: #5865f2;"></i>
            <span>Discord</span>
            <span class="account-status disconnected">未连接</span>
          </div>
          <div class="account-item">
            <i class="fa fa-github"></i>
            <span>GitHub</span>
            <span class="account-status disconnected">未连接</span>
          </div>
        </div>
        <p class="text-secondary mt-2">
          <small>在登录页面使用 OAuth 登录后将自动关联</small>
        </p>
      </div>

      <FluentMessage v-if="message" :type="messageType">{{ message }}</FluentMessage>
    </div>
  </div>
</template>

<script setup lang="ts">
const { user } = useUser()
const { success, error: showError } = useToast()

const displayName = ref('')
const bio = ref('')
const location = ref('')
const website = ref('')
const email = ref('')
const bannerUrl = ref('')

const message = ref('')
const messageType = ref<'success' | 'error'>('success')

onMounted(async () => {
  try {
    const res: any = await $fetch('/api/user/profile')
    if (res.data) {
      displayName.value = res.data.display_name || ''
      bio.value = res.data.bio || ''
      location.value = res.data.location || ''
      website.value = res.data.website || ''
      email.value = res.data.email || ''
      bannerUrl.value = res.data.banner_url || ''
    }
  } catch {}
})

async function saveProfile() {
  try {
    await $fetch('/api/user/profile', {
      method: 'PUT',
      body: { display_name: displayName.value, bio: bio.value, location: location.value, website: website.value },
    })
    success('已保存')
  } catch { showError('保存失败') }
}

async function saveBanner() {
  if (!bannerUrl.value) return
  try {
    await $fetch('/api/user/banner', {
      method: 'POST',
      body: { bannerUrl: bannerUrl.value },
    })
    success('封面已保存')
  } catch { showError('保存失败') }
}

async function saveEmail() {
  try {
    await $fetch('/api/auth/send-verification', {
      method: 'POST',
      body: { email: email.value },
    })
    success('验证邮件已发送')
  } catch { showError('发送失败') }
}

async function sendVerification() {
  if (!email.value) { showError('请先输入邮箱'); return }
  try {
    await $fetch('/api/auth/send-verification', {
      method: 'POST',
      body: { email: email.value },
    })
    success('验证邮件已发送')
  } catch { showError('发送失败') }
}
</script>

<style scoped>
.settings-page { max-width: 700px; margin: 0 auto; padding: var(--fluent-spacing-2xl) var(--fluent-spacing-xl); }
.settings-page h1 { margin-bottom: var(--fluent-spacing-2xl); font-size: var(--fluent-font-size-title-large); }
.settings-page h1 i { color: var(--fluent-accent); margin-right: var(--fluent-spacing-sm); }
.settings-section { padding: var(--fluent-spacing-xl); margin-bottom: var(--fluent-spacing-xl); }
.settings-section h2 { font-size: var(--fluent-font-size-title); margin: 0 0 var(--fluent-spacing-lg); }
.mt-3 { margin-top: var(--fluent-spacing-lg); }
.mt-2 { margin-top: var(--fluent-spacing-md); }
.banner-preview { margin-bottom: var(--fluent-spacing-md); }
.banner-preview img { width: 100%; max-height: 200px; object-fit: cover; border-radius: var(--fluent-radius-lg); }
.banner-upload { display: flex; gap: var(--fluent-spacing-sm); align-items: flex-end; }
.settings-actions { display: flex; gap: var(--fluent-spacing-sm); margin-top: var(--fluent-spacing-xl); }
.connected-accounts { display: flex; flex-direction: column; gap: var(--fluent-spacing-sm); }
.account-item { display: flex; align-items: center; gap: var(--fluent-spacing-md); padding: var(--fluent-spacing-sm) 0; }
.account-item i { font-size: 20px; width: 24px; }
.account-item span { flex: 1; }
.account-status { font-size: var(--fluent-font-size-caption); font-weight: 600; }
.account-status.disconnected { color: var(--fluent-text-tertiary); }
.text-secondary { color: var(--fluent-text-secondary); }
</style>
