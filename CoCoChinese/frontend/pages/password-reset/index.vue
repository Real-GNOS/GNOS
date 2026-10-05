<template>
  <div class="auth-page">
    <div class="auth-container fluent-card">
      <div class="auth-header">
        <i class="fa fa-lock fa-3x" style="color: var(--fluent-accent); margin-bottom: 16px;"></i>
        <h1 class="auth-title">忘记密码</h1>
        <p class="auth-desc">输入注册邮箱，我们将发送重置链接</p>
      </div>

      <FluentMessage v-if="message" :type="messageType" class="mb-3">{{ message }}</FluentMessage>

      <template v-if="!token">
        <form @submit.prevent="sendReset">
          <FluentTextField v-model="email" label="邮箱" type="email" placeholder="请输入注册邮箱" required />
          <FluentButton variant="primary" block :loading="loading">发送重置链接</FluentButton>
        </form>
      </template>

      <template v-else>
        <form @submit.prevent="doReset">
          <FluentTextField v-model="newPassword" label="新密码" type="password" placeholder="至少8位" required />
          <FluentTextField v-model="confirmPassword" label="确认密码" type="password" placeholder="再次输入新密码" required />
          <FluentButton variant="primary" block :loading="loading">重置密码</FluentButton>
        </form>
      </template>

      <p class="auth-footer mt-3">
        <NuxtLink to="/login">返回登录</NuxtLink>
      </p>
    </div>
  </div>
</template>

<script setup lang="ts">
const route = useRoute()
const token = ref(route.query.token as string || '')

const email = ref('')
const newPassword = ref('')
const confirmPassword = ref('')
const message = ref('')
const messageType = ref<'info' | 'success' | 'error'>('info')
const loading = ref(false)

async function sendReset() {
  loading.value = true
  message.value = ''
  try {
    await $fetch('/api/auth/forgot-password', {
      method: 'POST',
      body: { email: email.value },
    })
    message.value = '如果该邮箱已注册，重置链接已发送。请检查您的邮箱。'
    messageType.value = 'success'
  } catch (e: any) {
    message.value = e?.data?.message || '发送失败'
    messageType.value = 'error'
  } finally {
    loading.value = false
  }
}

async function doReset() {
  if (newPassword.value !== confirmPassword.value) {
    message.value = '两次密码输入不一致'
    messageType.value = 'error'
    return
  }
  loading.value = true
  message.value = ''
  try {
    await $fetch('/api/auth/reset-password', {
      method: 'POST',
      body: { token: token.value, password: newPassword.value },
    })
    message.value = '密码已重置！即将跳转到登录页...'
    messageType.value = 'success'
    setTimeout(() => navigateTo('/login'), 2000)
  } catch (e: any) {
    message.value = e?.data?.message || '重置失败'
    messageType.value = 'error'
  } finally {
    loading.value = false
  }
}
</script>

<style scoped>
.auth-page {
  min-height: 100vh;
  background: var(--fluent-bg);
  display: flex; align-items: center; justify-content: center;
  padding: var(--fluent-spacing-xl);
}
.auth-container {
  width: 100%; max-width: 400px;
  padding: var(--fluent-spacing-3xl);
  animation: fluent-slide-up var(--fluent-animation-duration) var(--fluent-animation-easing);
}
.auth-header { text-align: center; margin-bottom: var(--fluent-spacing-2xl); }
.auth-title { font-size: var(--fluent-font-size-title-large); font-weight: 700; margin: 0; color: var(--fluent-text); }
.auth-desc { color: var(--fluent-text-secondary); margin: var(--fluent-spacing-xs) 0 0; }
.auth-footer { text-align: center; color: var(--fluent-text-secondary); }
.auth-footer a { font-weight: 600; }
</style>
