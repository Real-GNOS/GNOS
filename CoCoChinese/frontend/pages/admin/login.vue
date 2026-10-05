<template>
  <div class="login-page">
    <div class="login-bg"></div>
    <div class="login-card">
      <div class="login-header">
        <img src="/images/little_logo.png" alt="" class="login-logo" />
        <h1>Cocokalo 管理后台</h1>
        <p>请使用管理员账号登录</p>
      </div>
      <el-form ref="formRef" :model="form" :rules="rules" class="login-form" @submit.prevent="handleLogin">
        <el-form-item prop="username">
          <el-input v-model="form.username" size="large" placeholder="管理员用户名" :prefix-icon="User" />
        </el-form-item>
        <el-form-item prop="password">
          <el-input v-model="form.password" size="large" type="password" placeholder="密码" show-password :prefix-icon="Lock" @keyup.enter="handleLogin" />
        </el-form-item>
        <el-form-item>
          <el-button type="primary" size="large" :loading="loading" class="login-btn" @click="handleLogin">
            {{ loading ? '登录中...' : '登 录' }}
          </el-button>
        </el-form-item>
      </el-form>
    </div>
  </div>
</template>

<script setup lang="ts">
import { User, Lock } from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'

definePageMeta({ layout: false })

const formRef = ref()
const loading = ref(false)
const form = reactive({ username: '', password: '' })
const rules = {
  username: [{ required: true, message: '请输入用户名', trigger: 'blur' }],
  password: [{ required: true, message: '请输入密码', trigger: 'blur' }],
}

async function handleLogin() {
  try { await formRef.value?.validate() } catch { return }
  loading.value = true
  try {
    const res: any = await $fetch('/api/admin/login', {
      method: 'POST',
      body: { username: form.username, password: form.password },
    })
    ElMessage.success('登录成功')
    if (res.redirect) navigateTo(res.redirect)
    else navigateTo('/admin/dashboard')
  } catch (e: any) {
    ElMessage.error(e?.data?.message || '登录失败，请检查用户名和密码')
  } finally {
    loading.value = false
  }
}
</script>

<style scoped>
.login-page {
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  background: linear-gradient(135deg, #1d1e2c 0%, #2d1b69 50%, #1d1e2c 100%);
  position: relative;
  overflow: hidden;
}
.login-bg {
  position: absolute;
  inset: 0;
  background:
    radial-gradient(circle at 20% 50%, rgba(108, 92, 231, 0.15) 0%, transparent 50%),
    radial-gradient(circle at 80% 20%, rgba(162, 155, 254, 0.1) 0%, transparent 50%);
}
.login-card {
  position: relative;
  width: 400px;
  background: #fff;
  border-radius: 16px;
  padding: 48px 40px;
  box-shadow: 0 20px 60px rgba(0,0,0,0.3);
}
.login-header { text-align: center; margin-bottom: 32px; }
.login-logo { width: 56px; height: 56px; border-radius: 12px; margin-bottom: 16px; }
.login-header h1 { margin: 0; font-size: 22px; color: #1d1e2c; font-weight: 700; }
.login-header p { margin: 8px 0 0; color: #909399; font-size: 14px; }
.login-form { margin-top: 8px; }
.login-btn { width: 100%; height: 44px; font-size: 16px; border-radius: 8px; }
</style>
