<template>
  <div class="container py-5">
    <div class="row justify-content-center">
      <div class="col-md-6 col-lg-4">
        <div class="card shadow-sm">
          <div class="card-body p-4">
            <h2 class="text-center mb-4">登录</h2>
            <div v-if="error" class="alert alert-danger">{{ error }}</div>
            <form @submit.prevent="handleLogin">
              <div class="mb-3">
                <label class="form-label">用户名</label>
                <input v-model="username" type="text" class="form-control" required>
              </div>
              <div class="mb-3">
                <label class="form-label">密码</label>
                <input v-model="password" type="password" class="form-control" required>
              </div>
              <button type="submit" class="btn btn-primary w-100" :disabled="loading">
                {{ loading ? '登录中...' : '登录' }}
              </button>
            </form>
            <p class="text-center mt-3 mb-0">
              还没有账号？<NuxtLink to="/register">注册</NuxtLink>
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
const loading = ref(false)

async function handleLogin() {
  loading.value = true
  error.value = ''
  try {
    await $fetch('/api/auth/login', {
      method: 'POST',
      body: { username: username.value, password: password.value }
    })
    navigateTo('/', { external: true })
  } catch (e) {
    error.value = '登录失败，请检查用户名和密码'
  } finally {
    loading.value = false
  }
}
</script>
