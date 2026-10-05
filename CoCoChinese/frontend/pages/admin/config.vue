<template>
  <div class="admin-page">
    <h2 class="page-title">站点配置</h2>

    <el-form
      ref="formRef"
      :model="config"
      label-position="top"
      class="config-form"
      @submit.prevent="saveConfig"
    >
      <el-row :gutter="20">
        <el-col :xs="24" :md="12">
          <el-card shadow="never" class="config-card">
            <template #header><span>基本信息</span></template>
            <el-form-item label="站点名称">
              <el-input v-model="config.site_name" placeholder="Cocokalo" />
            </el-form-item>
            <el-form-item label="站点描述">
              <el-input
                v-model="config.site_description"
                type="textarea"
                :rows="3"
                placeholder="站点描述"
              />
            </el-form-item>
          </el-card>
        </el-col>

        <el-col :xs="24" :md="12">
          <el-card shadow="never" class="config-card">
            <template #header><span>功能开关</span></template>
            <el-form-item label="开放注册">
              <el-switch v-model="config.registration_open" />
            </el-form-item>
            <el-form-item label="视频需要审核">
              <el-switch v-model="config.video_approval" />
            </el-form-item>
            <el-form-item label="维护模式">
              <el-switch v-model="config.maintenance_mode" active-color="#e6a23c" />
            </el-form-item>
          </el-card>
        </el-col>

        <el-col :xs="24" :md="12">
          <el-card shadow="never" class="config-card">
            <template #header><span>上传与显示</span></template>
            <el-form-item label="首页轮播数量">
              <el-input-number v-model="config.home_carousel_limit" :min="0" :max="20" />
            </el-form-item>
            <el-form-item label="最大上传大小 (MB)">
              <el-input-number v-model="config.max_upload_size" :min="1" :max="5000" />
            </el-form-item>
          </el-card>
        </el-col>

        <el-col :xs="24" :md="12">
          <el-card shadow="never" class="config-card">
            <template #header><span>用户设置</span></template>
            <el-form-item label="默认用户角色">
              <el-select v-model="config.default_user_role" style="width: 100%">
                <el-option label="普通用户" value="user" />
                <el-option label="投稿者" value="contributor" />
              </el-select>
            </el-form-item>
          </el-card>
        </el-col>
      </el-row>

      <div class="form-actions">
        <el-button type="primary" :loading="saving" @click="saveConfig">保存配置</el-button>
      </div>
    </el-form>
  </div>
</template>

<script setup lang="ts">
import { ElMessage } from 'element-plus'

definePageMeta({ layout: 'admin', middleware: 'admin-auth' })

interface Config {
  site_name: string
  site_description: string
  registration_open: boolean
  home_carousel_limit: number
  default_user_role: string
  video_approval: boolean
  max_upload_size: number
  maintenance_mode: boolean
}

const config = reactive<Config>({
  site_name: '',
  site_description: '',
  registration_open: true,
  home_carousel_limit: 5,
  default_user_role: 'user',
  video_approval: true,
  max_upload_size: 500,
  maintenance_mode: false,
})

const saving = ref(false)

onMounted(async () => {
  try {
    const res = await $fetch<any>('/api/admin/config')
    Object.assign(config, {
      site_name: res.site_name || '',
      site_description: res.site_description || '',
      registration_open: res.registration_open !== false,
      home_carousel_limit: Number(res.home_carousel_limit) || 5,
      default_user_role: res.default_user_role || 'user',
      video_approval: res.video_approval !== false,
      max_upload_size: Number(res.max_upload_size) || 500,
      maintenance_mode: res.maintenance_mode === true,
    })
  } catch {
    ElMessage.error('加载配置失败')
  }
})

async function saveConfig() {
  saving.value = true
  try {
    await $fetch('/api/admin/config', {
      method: 'PUT',
      body: { ...config },
    })
    ElMessage.success('配置已保存')
  } catch (e: any) {
    ElMessage.error(e?.data?.message || '保存失败')
  } finally {
    saving.value = false
  }
}
</script>

<style scoped>
.admin-page { max-width: 960px; }
.page-title { font-size: 20px; font-weight: 600; margin-bottom: 24px; }
.config-card { border-radius: 10px; }
.config-card :deep(.el-card__header) {
  padding: 14px 20px;
  font-weight: 600;
  font-size: 15px;
  border-bottom: 1px solid #f0f0f0;
}
.form-actions { margin-top: 24px; }
</style>
