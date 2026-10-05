<template>
  <div class="admin-page">
    <h2 class="page-title">操作审计日志</h2>

    <el-card shadow="never" class="audit-card">
      <template #header>
        <div class="audit-header">
          <el-input
            v-model="filter.action"
            placeholder="按操作筛选（如 POST /api/admin/videos）"
            clearable
            style="width: 360px"
            @keyup.enter="loadLogs(1)"
            @clear="loadLogs(1)"
          >
            <template #append>
              <el-button @click="loadLogs(1)"><el-icon><Search /></el-icon></el-button>
            </template>
          </el-input>
          <el-button type="primary" @click="loadLogs(1)">刷新</el-button>
        </div>
      </template>

      <el-table v-loading="loading" :data="logs" border stripe style="width: 100%">
        <el-table-column prop="id" label="ID" width="70" align="center" />
        <el-table-column label="操作人" width="160">
          <template #default="{ row }">
            <span class="op-user">
              <el-avatar :size="24" :src="row.avatar_url" />
              {{ row.username || `#${row.user_id}` }}
            </span>
          </template>
        </el-table-column>
        <el-table-column prop="action" label="操作" min-width="220" show-overflow-tooltip />
        <el-table-column label="目标" width="140">
          <template #default="{ row }">
            <el-tag size="small">{{ row.target_type || '-' }}</el-tag>
            <span v-if="row.target_id" class="target-id">#{{ row.target_id }}</span>
          </template>
        </el-table-column>
        <el-table-column prop="ip_address" label="IP" width="130" />
        <el-table-column label="详情" min-width="220" show-overflow-tooltip>
          <template #default="{ row }">
            <span class="details">{{ row.details || '-' }}</span>
          </template>
        </el-table-column>
        <el-table-column label="时间" width="170">
          <template #default="{ row }">
            {{ formatTime(row.created_at) }}
          </template>
        </el-table-column>
      </el-table>

      <el-empty
        v-if="!loading && !logs.length"
        description="暂无审计日志"
      />

      <div class="pagination-row">
        <el-pagination
          v-model:current-page="page"
          :page-size="size"
          :total="total"
          layout="total, prev, pager, next"
          @current-change="loadLogs"
        />
      </div>
    </el-card>
  </div>
</template>

<script setup lang="ts">
import { Search } from '@element-plus/icons-vue'

definePageMeta({ layout: 'admin', middleware: 'admin-auth' })

const loading = ref(false)
const logs = ref<any[]>([])
const total = ref(0)
const page = ref(1)
const size = 20
const filter = reactive({ action: '' })

function formatTime(ts: string | null | undefined) {
  if (!ts) return '-'
  const d = new Date(ts)
  if (Number.isNaN(d.getTime())) return ts
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
}

async function loadLogs(p: number = page.value) {
  loading.value = true
  try {
    const params = new URLSearchParams({
      page: String(p),
      size: String(size),
    })
    if (filter.action) params.set('action', filter.action)
    const res: any = await $fetch(`/api/admin/audit?${params.toString()}`)
    logs.value = res?.logs || []
    total.value = res?.total || 0
    page.value = p
  } catch (err: any) {
    ElMessage.error(err?.data?.message || '加载审计日志失败')
  } finally {
    loading.value = false
  }
}

onMounted(() => loadLogs(1))
</script>

<style scoped>
.admin-page { max-width: 1200px; }
.page-title { font-size: 20px; font-weight: 600; margin-bottom: 24px; }
.audit-card { border-radius: 10px; }
.audit-header { display: flex; gap: 12px; align-items: center; }
.op-user { display: inline-flex; align-items: center; gap: 6px; }
.target-id { margin-left: 6px; color: #909399; font-size: 12px; }
.details { color: #606266; font-size: 13px; }
.pagination-row { margin-top: 16px; display: flex; justify-content: flex-end; }
</style>
