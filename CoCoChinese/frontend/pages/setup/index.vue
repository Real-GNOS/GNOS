<template>
  <div class="setup-page">
    <div class="setup-container">
      <div class="setup-header">
        <img src="/images/little_logo.png" alt="Cocokalo" class="setup-logo" />
        <h1 class="setup-title">Cocokalo 安装向导</h1>
        <p class="setup-desc">检测环境并自动完成配置</p>
      </div>

      <!-- Step indicator -->
      <div class="setup-steps">
        <div v-for="(s, i) in steps" :key="i" :class="['setup-step', { active: currentStep === i, done: currentStep > i }]">
          <div class="step-number">{{ currentStep > i ? '✓' : i + 1 }}</div>
          <span class="step-label">{{ s }}</span>
        </div>
      </div>

      <!-- Step 0: Environment check -->
      <div v-if="currentStep === 0" class="setup-panel fluent-card">
        <h2>环境检测</h2>
        <div class="env-checks">
          <div v-for="(val, key) in status.checks" :key="key" class="env-check-item">
            <span class="env-check-icon" :class="val ? 'pass' : 'fail'">
              <i :class="val ? 'fa fa-check-circle' : 'fa fa-times-circle'"></i>
            </span>
            <span class="env-check-name">{{ checkLabels[key as keyof typeof checkLabels] || key }}</span>
            <span v-if="!val" class="env-check-status">未安装</span>
          </div>
          <div class="env-check-item">
            <span class="env-check-icon" :class="status.envExists ? 'pass' : 'fail'">
              <i :class="status.envExists ? 'fa fa-check-circle' : 'fa fa-times-circle'"></i>
            </span>
            <span class="env-check-name">环境配置 (.env)</span>
            <span v-if="!status.envExists" class="env-check-status">未配置</span>
          </div>
        </div>
        <div class="setup-actions">
          <button class="fluent-btn fluent-btn-primary" @click="runCheck">重新检测</button>
          <button class="fluent-btn fluent-btn-primary" @click="currentStep = 1">下一步</button>
        </div>
      </div>

      <!-- Step 1: DB config -->
      <div v-else-if="currentStep === 1" class="setup-panel fluent-card">
        <h2>站点与数据库配置</h2>
        <p class="text-secondary">配置站点基本信息和数据库连接</p>
        <div class="setup-form">
          <div class="form-group">
            <label>站点名称</label>
            <input v-model="config.siteName" class="fluent-input" placeholder="Cocokalo" />
          </div>
          <div class="form-group">
            <label>站点 URL</label>
            <input v-model="config.siteUrl" class="fluent-input" placeholder="http://localhost:3000" />
          </div>
          <div class="form-section-divider">数据库连接</div>
          <div class="form-group">
            <label>PostgreSQL 主机</label>
            <input v-model="config.dbHost" class="fluent-input" placeholder="localhost" />
          </div>
          <div class="form-group">
            <label>PostgreSQL 端口</label>
            <input v-model="config.dbPort" class="fluent-input" placeholder="5432" />
          </div>
          <div class="form-group">
            <label>数据库名</label>
            <input v-model="config.dbName" class="fluent-input" placeholder="cocokalo" />
          </div>
          <div class="form-group">
            <label>用户名</label>
            <input v-model="config.dbUser" class="fluent-input" placeholder="postgres" />
          </div>
          <div class="form-group">
            <label>密码</label>
            <input v-model="config.dbPassword" class="fluent-input" type="password" placeholder="123" />
          </div>
          <div class="form-group">
            <label>Redis 连接</label>
            <input v-model="config.redisUrl" class="fluent-input" placeholder="redis://localhost:6379" />
          </div>
        </div>
        <div class="setup-actions">
          <button class="fluent-btn fluent-btn-secondary" @click="currentStep = 0">上一步</button>
          <button class="fluent-btn fluent-btn-primary" @click="currentStep = 2">下一步</button>
        </div>
      </div>

      <!-- Step 2: Admin config -->
      <div v-else-if="currentStep === 2" class="setup-panel fluent-card">
        <h2>管理员配置</h2>
        <p class="text-secondary">创建默认管理员账号</p>
        <div class="setup-form">
          <div class="form-group">
            <label>管理员用户名</label>
            <input v-model="config.adminUser" class="fluent-input" placeholder="admin" />
          </div>
          <div class="form-group">
            <label>管理员密码</label>
            <input v-model="config.adminPassword" class="fluent-input" type="password" placeholder="admin123" />
          </div>
        </div>
        <div class="setup-actions">
          <button class="fluent-btn fluent-btn-secondary" @click="currentStep = 1">上一步</button>
          <button class="fluent-btn fluent-btn-primary" @click="doInstall">开始安装</button>
        </div>
      </div>

      <!-- Step 3: Installing -->
      <div v-else-if="currentStep === 3" class="setup-panel fluent-card">
        <h2>正在安装...</h2>
        <div class="install-progress">
          <div v-for="(msg, i) in installLogs" :key="i" class="install-log" :class="msg.type">
            <i :class="msg.type === 'done' ? 'fa fa-check-circle text-success' : msg.type === 'error' ? 'fa fa-times-circle text-danger' : 'fa fa-circle-o-notch fa-spin'"></i>
            {{ msg.text }}
          </div>
        </div>
        <div v-if="installDone" class="setup-done">
          <i class="fa fa-check-circle fa-4x text-success"></i>
          <h2>安装完成！</h2>
          <p>Cocokalo 已成功配置并可以使用</p>
          <button class="fluent-btn fluent-btn-primary" @click="goToHome">进入首页</button>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
const currentStep = ref(0)
const steps = ['环境检测', '数据库配置', '管理员设置', '安装']
const installLogs = ref<{ text: string; type: string }[]>([])
const installDone = ref(false)

const checkLabels: Record<string, string> = {
  node: 'Node.js', npm: 'npm', pnpm: 'pnpm', git: 'Git',
  ffmpeg: 'FFmpeg', ffprobe: 'FFprobe',
  postgres: 'PostgreSQL', redis: 'Redis',
}

const status = ref({
  checks: {} as Record<string, boolean>,
  envExists: false,
  dbConnected: false,
  redisConnected: false,
})

const config = reactive({
  siteName: 'Cocokalo',
  dbHost: 'localhost',
  dbPort: '5432',
  dbName: 'cocokalo',
  dbUser: 'postgres',
  dbPassword: '123',
  redisUrl: 'redis://localhost:6379',
  siteUrl: 'http://localhost:3000',
  adminUser: 'admin',
  adminPassword: 'admin123',
})

async function runCheck() {
  try {
    const res = await $fetch('/api/setup/status')
    status.value = res as any
  } catch (e) {
    console.error('Check failed:', e)
  }
}

onMounted(() => runCheck())

async function doInstall() {
  currentStep.value = 3
  installLogs.value = []

  const log = (text: string, type = 'info') => installLogs.value.push({ text, type })

  try {
    log('正在创建配置文件...')
    await $fetch('/api/setup/create-env', {
      method: 'POST',
      body: config,
    })
    log('配置文件已创建', 'done')

    log('正在安装依赖...')
    await $fetch('/api/setup/install-deps', { method: 'POST' })
    log('依赖安装完成', 'done')

    log('正在生成 Prisma 客户端...')
    await $fetch('/api/setup/prisma-gen', { method: 'POST' })
    log('Prisma 客户端已生成', 'done')

    log('正在初始化数据库...')
    await $fetch('/api/setup/db-migrate', { method: 'POST' })
    log('数据库初始化完成', 'done')

    log('正在创建管理员账号...')
    await $fetch('/api/setup/create-admin', {
      method: 'POST',
      body: { username: config.adminUser, password: config.adminPassword },
    })
    log('管理员账号已创建', 'done')

    log('正在保存站点配置...')
    await $fetch('/api/setup/complete', {
      method: 'POST',
      body: { siteName: config.siteName },
    })
    log('站点配置已保存', 'done')

    installDone.value = true
  } catch (e: any) {
    log(`安装失败: ${e.data?.message || e.message}`, 'error')
  }
}

function goToHome() {
  window.location.href = '/'
}
</script>

<style scoped>
.setup-page {
  min-height: 100vh;
  background: var(--fluent-bg);
  display: flex;
  align-items: flex-start;
  justify-content: center;
  padding: 60px 20px;
}
.setup-container {
  width: 100%;
  max-width: 600px;
}
.setup-header {
  text-align: center;
  margin-bottom: var(--fluent-spacing-3xl);
}
.setup-logo { width: 64px; height: 64px; margin-bottom: 16px; }
.setup-title {
  font-size: var(--fluent-font-size-display);
  font-weight: 700;
  color: var(--fluent-text);
  margin: 0 0 8px;
}
.setup-desc { color: var(--fluent-text-secondary); margin: 0; }
.setup-steps {
  display: flex;
  justify-content: center;
  gap: var(--fluent-spacing-xl);
  margin-bottom: var(--fluent-spacing-3xl);
}
.setup-step {
  display: flex; flex-direction: column;
  align-items: center; gap: var(--fluent-spacing-xs);
  opacity: 0.4;
  transition: opacity var(--fluent-animation-duration) var(--fluent-animation-easing);
}
.setup-step.active, .setup-step.done { opacity: 1; }
.step-number {
  width: 32px; height: 32px;
  border-radius: var(--fluent-radius-circular);
  background: var(--fluent-border);
  color: var(--fluent-text-secondary);
  display: flex; align-items: center; justify-content: center;
  font-weight: 700; font-size: 14px;
}
.setup-step.active .step-number { background: var(--fluent-accent); color: white; }
.setup-step.done .step-number { background: var(--fluent-status-success); color: white; }
.step-label { font-size: var(--fluent-font-size-caption); font-weight: 600; color: var(--fluent-text-secondary); }
.setup-panel { padding: var(--fluent-spacing-3xl); }
.setup-panel h2 { font-size: var(--fluent-font-size-title-large); margin: 0 0 var(--fluent-spacing-lg); }
.env-checks { display: flex; flex-direction: column; gap: var(--fluent-spacing-sm); margin-bottom: var(--fluent-spacing-xl); }
.env-check-item { display: flex; align-items: center; gap: var(--fluent-spacing-sm); padding: 8px 12px; border-radius: var(--fluent-radius-md); background: var(--fluent-bg-subtle); }
.env-check-icon { font-size: 18px; }
.env-check-icon.pass { color: var(--fluent-status-success); }
.env-check-icon.fail { color: var(--fluent-status-error); }
.env-check-name { flex: 1; font-weight: 500; }
.env-check-status { font-size: var(--fluent-font-size-caption); color: var(--fluent-status-error); font-weight: 600; }
.setup-form { display: flex; flex-direction: column; gap: var(--fluent-spacing-md); margin-bottom: var(--fluent-spacing-xl); }
.form-section-divider { font-size: var(--fluent-font-size-caption); font-weight: 700; color: var(--fluent-accent); padding: var(--fluent-spacing-sm) 0 var(--fluent-spacing-xs); border-top: 1px solid var(--fluent-border); margin-top: var(--fluent-spacing-sm); }
.form-group label { font-size: var(--fluent-font-size-caption); font-weight: 600; color: var(--fluent-text-secondary); display: block; margin-bottom: 4px; }
.setup-actions { display: flex; gap: var(--fluent-spacing-sm); justify-content: flex-end; }
.text-secondary { color: var(--fluent-text-secondary); }
.install-progress { display: flex; flex-direction: column; gap: var(--fluent-spacing-sm); margin: var(--fluent-spacing-xl) 0; }
.install-log { display: flex; align-items: center; gap: var(--fluent-spacing-sm); padding: 8px; border-radius: var(--fluent-radius-md); }
.install-log.done { background: var(--fluent-status-success-bg); }
.install-log.error { background: var(--fluent-status-error-bg); }
.setup-done { text-align: center; padding: var(--fluent-spacing-3xl) 0; }
.setup-done h2 { margin: var(--fluent-spacing-lg) 0 var(--fluent-spacing-sm) !important; }
.setup-done p { color: var(--fluent-text-secondary); margin-bottom: var(--fluent-spacing-xl); }
</style>
