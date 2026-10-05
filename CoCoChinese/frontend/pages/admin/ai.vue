<template>
  <div class="admin-page">
    <h2 class="page-title">AI 审核配置</h2>

    <el-form
      :model="config"
      label-position="top"
      class="ai-form"
      @submit.prevent="saveConfig"
    >
      <el-row :gutter="20">
        <el-col :xs="24" :md="12">
          <el-card shadow="never" class="config-card">
            <template #header><span>API 密钥配置</span></template>
            <el-form-item label="AI 服务商">
              <el-select v-model="config.ai_review_provider" style="width: 100%" @change="onProviderChange">
                <el-option label="OpenAI" value="openai" />
                <el-option label="DeepSeek" value="deepseek" />
                <el-option label="智谱 AI (GLM)" value="zhipu" />
                <el-option label="Moonshot (Kimi)" value="moonshot" />
                <el-option label="自定义 (兼容 OpenAI 接口)" value="custom" />
              </el-select>
            </el-form-item>
            <el-form-item label="API Key">
              <el-input
                v-model="config.ai_review_api_key"
                type="password"
                show-password
                placeholder="sk-..."
              />
              <div class="field-tip">密钥仅保存在服务器数据库中，不会泄露</div>
            </el-form-item>
            <el-form-item label="API Endpoint">
              <el-input v-model="config.ai_review_endpoint" :placeholder="endpointPlaceholder" />
            </el-form-item>
            <el-form-item label="模型名称">
              <el-input v-model="config.ai_review_model" :placeholder="modelPlaceholder" />
            </el-form-item>
          </el-card>
        </el-col>

        <el-col :xs="24" :md="12">
          <el-card shadow="never" class="config-card">
            <template #header><span>审核规则</span></template>
            <el-form-item label="自动审核评论">
              <el-switch v-model="config.ai_review_comments" />
            </el-form-item>
            <el-form-item label="自动审核弹幕">
              <el-switch v-model="config.ai_review_danmaku" />
            </el-form-item>
            <el-form-item label="自动审核投稿标题">
              <el-switch v-model="config.ai_review_uploads" />
            </el-form-item>
            <el-form-item label="敏感内容自动删除">
              <el-switch v-model="config.ai_auto_delete" active-color="#e6a23c" />
              <div class="field-tip">关闭时仅标记，开启时自动删除</div>
            </el-form-item>
          </el-card>

          <el-card shadow="never" class="config-card" style="margin-top: 20px">
            <template #header><span>测试审核</span></template>
            <el-form-item label="输入测试内容">
              <el-input
                v-model="testContent"
                type="textarea"
                :rows="3"
                placeholder="输入一段文字来测试AI审核..."
              />
            </el-form-item>
            <el-button type="primary" :loading="testing" @click="testReview">
              {{ testing ? '审核中...' : '测试审核' }}
            </el-button>
            <div v-if="testResult" class="test-result" :class="testResult.approved ? 'approved' : 'rejected'">
              <el-icon :size="18">
                <CircleCheckFilled v-if="testResult.approved" />
                <CircleCloseFilled v-else />
              </el-icon>
              <span>{{ testResult.approved ? '通过' : '未通过' }} - {{ testResult.reason }}</span>
              <span v-if="testResult.confidence" class="confidence">
                置信度: {{ (testResult.confidence * 100).toFixed(0) }}%
              </span>
            </div>
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
import { CircleCheckFilled, CircleCloseFilled } from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'

definePageMeta({ layout: 'admin', middleware: 'admin-auth' })

interface AIConfig {
  ai_review_provider: string
  ai_review_api_key: string
  ai_review_endpoint: string
  ai_review_model: string
  ai_review_comments: boolean
  ai_review_danmaku: boolean
  ai_review_uploads: boolean
  ai_auto_delete: boolean
}

const config = reactive<AIConfig>({
  ai_review_provider: 'openai',
  ai_review_api_key: '',
  ai_review_endpoint: '',
  ai_review_model: '',
  ai_review_comments: true,
  ai_review_danmaku: true,
  ai_review_uploads: true,
  ai_auto_delete: false,
})

const testContent = ref('')
const testing = ref(false)
const testResult = ref<{ approved: boolean; reason: string; confidence?: number } | null>(null)
const saving = ref(false)

const defaultEndpoints: Record<string, string> = {
  openai: 'https://api.openai.com/v1/chat/completions',
  deepseek: 'https://api.deepseek.com/v1/chat/completions',
  zhipu: 'https://open.bigmodel.cn/api/paas/v4/chat/completions',
  moonshot: 'https://api.moonshot.cn/v1/chat/completions',
  custom: 'https://your-api.com/v1/chat/completions',
}

const defaultModels: Record<string, string> = {
  openai: 'gpt-3.5-turbo',
  deepseek: 'deepseek-chat',
  zhipu: 'glm-4-flash',
  moonshot: 'moonshot-v1-8k',
  custom: 'model-name',
}

const endpointPlaceholder = computed(() => defaultEndpoints[config.ai_review_provider] || defaultEndpoints.openai)
const modelPlaceholder = computed(() => defaultModels[config.ai_review_provider] || defaultModels.openai)

function onProviderChange() {
  config.ai_review_endpoint = ''
  config.ai_review_model = ''
}

onMounted(async () => {
  try {
    const res = await $fetch<any>('/api/admin/config')
    if (res.ai_review_provider) config.ai_review_provider = res.ai_review_provider
    if (res.ai_review_api_key) config.ai_review_api_key = res.ai_review_api_key
    if (res.ai_review_endpoint) config.ai_review_endpoint = res.ai_review_endpoint
    if (res.ai_review_model) config.ai_review_model = res.ai_review_model
    config.ai_review_comments = res.ai_review_comments !== false
    config.ai_review_danmaku = res.ai_review_danmaku !== false
    config.ai_review_uploads = res.ai_review_uploads !== false
    config.ai_auto_delete = res.ai_auto_delete === true
  } catch {
    ElMessage.error('加载配置失败')
  }
})

async function saveConfig() {
  saving.value = true
  try {
    const body: Record<string, any> = { ...config }
    if (!body.ai_review_endpoint) body.ai_review_endpoint = endpointPlaceholder.value
    if (!body.ai_review_model) body.ai_review_model = modelPlaceholder.value
    await $fetch('/api/admin/config', { method: 'PUT', body })
    ElMessage.success('AI 审核配置已保存')
  } catch (e: any) {
    ElMessage.error(e?.data?.message || '保存失败')
  } finally {
    saving.value = false
  }
}

async function testReview() {
  if (!testContent.value.trim()) return
  testing.value = true
  testResult.value = null
  try {
    const res = await $fetch<any>('/api/admin/ai-review', {
      method: 'POST',
      body: { content: testContent.value },
    })
    testResult.value = res
  } catch (e: any) {
    testResult.value = { approved: false, reason: e?.data?.message || '测试失败' }
  } finally {
    testing.value = false
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
.field-tip { font-size: 12px; color: #909399; margin-top: 4px; line-height: 1.5; }
.form-actions { margin-top: 24px; }

.test-result {
  margin-top: 16px;
  padding: 12px 16px;
  border-radius: 8px;
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 14px;
}
.test-result.approved { background: #f0f9eb; color: #67c23a; }
.test-result.rejected { background: #fef0f0; color: #f56c6c; }
.confidence { margin-left: auto; opacity: 0.7; }
</style>
