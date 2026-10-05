<template>
  <div class="container py-5">
    <div class="row justify-content-center">
      <div class="col-md-6 col-lg-4">
        <div class="card shadow-sm">
          <div class="card-body p-4">
            <h2 class="text-center mb-4">注册</h2>
            <div v-if="error" class="alert alert-danger">{{ error }}</div>
            <div v-if="success" class="alert alert-success">{{ success }}</div>
            <form @submit.prevent="handleRegister">
              <div class="mb-3">
                <label class="form-label">用户名</label>
                <input v-model="username" type="text" class="form-control" required minlength="4">
              </div>
              <div class="mb-3">
                <label class="form-label">密码</label>
                <input v-model="password" type="password" class="form-control" required minlength="8">
              </div>
              <button type="submit" class="btn btn-primary w-100" :disabled="loading">
                {{ loading ? '注册中...' : '注册' }}
              </button>
            </form>
            <p class="text-center mt-3 mb-0">
              已有账号？<NuxtLink to="/login">登录</NuxtLink>
            </p>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
const username = ref('')
const password = ref('')
const error = ref('')
const success = ref('')
const loading = ref(false)

async function handleRegister() {
  loading.value = true
  error.value = ''
  success.value = ''
  try {
    await $fetch('/api/auth/register', {
      method: 'POST',
      body: { username: username.value, password: password.value }
    })
    success.value = '注册成功！即将跳转到登录页...'
    setTimeout(() => navigateTo('/login'), 2000)
  } catch (e) {
    error.value = e?.data?.message || e?.message || '注册失败，请重试'
  } finally {
    loading.value = false
  }
}
</script>
