<template>
  <div class="admin-reports">
    <div class="page-header">
      <h2>举报管理</h2>
    </div>

    <el-card shadow="never" class="filter-card">
      <el-form :inline="true" @submit.prevent="handleSearch">
        <el-form-item label="状态">
          <el-select v-model="statusFilter" placeholder="全部" clearable @change="handleSearch">
            <el-option label="全部" value="all" />
            <el-option label="待处理" value="pending" />
            <el-option label="已通过" value="approved" />
            <el-option label="已驳回" value="rejected" />
          </el-select>
        </el-form-item>
        <el-form-item>
          <el-button type="primary" @click="handleSearch">筛选</el-button>
          <el-button @click="resetFilters">重置</el-button>
        </el-form-item>
      </el-form>
    </el-card>

    <el-card shadow="never">
      <el-table v-loading="loading" :data="reports" border stripe style="width: 100%">
        <el-table-column prop="id" label="ID" width="80" align="center" />

        <el-table-column label="举报者" width="140">
          <template #default="{ row }">{{ row.reporter_name }}</template>
        </el-table-column>

        <el-table-column label="类型" width="120" align="center">
          <template #default="{ row }">
            <el-tag size="small" effect="plain">{{ row.target_type }}</el-tag>
          </template>
        </el-table-column>

        <el-table-column label="目标ID" width="100" align="center">
          <template #default="{ row }">{{ row.target_id }}</template>
        </el-table-column>

        <el-table-column label="原因" min-width="200" show-overflow-tooltip>
          <template #default="{ row }">{{ row.reason }}</template>
        </el-table-column>

        <el-table-column label="状态" width="100" align="center">
          <template #default="{ row }">
            <el-tag
              :type="statusTagType(row.status)"
              size="small"
            >
              {{ statusLabel(row.status) }}
            </el-tag>
          </template>
        </el-table-column>

        <el-table-column label="时间" width="170" sortable>
          <template #default="{ row }">{{ formatDate(row.created_at) }}</template>
        </el-table-column>

        <el-table-column label="操作" width="200" fixed="right" align="center">
          <template #default="{ row }">
            <template v-if="row.status === 'pending'">
              <el-button type="success" size="small" plain @click="handleReport(row.id, 'approved')">
                通过
              </el-button>
              <el-button type="danger" size="small" plain @click="handleReport(row.id, 'rejected')">
                驳回
              </el-button>
            </template>
            <span v-else class="handled-text">
              由 {{ row.handler_name || '系统' }} 处理
            </span>
          </template>
        </el-table-column>

        <template #empty>
          <el-empty description="暂无举报" />
        </template>
      </el-table>

      <div class="pagination-wrap">
        <el-pagination
          v-model:current-page="page"
          v-model:page-size="size"
          :total="total"
          :page-sizes="[10, 20, 50]"
          layout="total, sizes, prev, pager, next, jumper"
          background
          @current-change="fetchReports"
          @size-change="handleSizeChange"
        />
      </div>
    </el-card>
  </div>
</template>

<script setup lang="ts">
import { ElMessage } from 'element-plus'
import type { ContentReport } from '~/types'

definePageMeta({ layout: 'admin', middleware: 'admin-auth' })

const { get, patch } = useRequest()
const { formatDate } = useAdmin()

const reports = ref<ContentReport[]>([])
const total = ref(0)
const page = ref(1)
const size = ref(20)
const loading = ref(false)
const statusFilter = ref('all')

function statusTagType(status: string) {
  const map: Record<string, string> = {
    pending: 'warning',
    approved: 'success',
    rejected: 'danger',
  }
  return map[status] || 'info'
}

function statusLabel(status: string) {
  const map: Record<string, string> = {
    pending: '待处理',
    approved: '已通过',
    rejected: '已驳回',
  }
  return map[status] || status
}

async function fetchReports() {
  loading.value = true
  try {
    const res = await get('/api/moderation/reports', {
      status: statusFilter.value,
      page: page.value,
      size: size.value,
    })
    if (res.success) {
      reports.value = res.reports || []
      total.value = res.total || 0
    }
  } catch {
    ElMessage.error('加载举报列表失败')
  } finally {
    loading.value = false
  }
}

function handleSearch() {
  page.value = 1
  fetchReports()
}

function resetFilters() {
  statusFilter.value = 'all'
  page.value = 1
  fetchReports()
}

function handleSizeChange() {
  page.value = 1
  fetchReports()
}

async function handleReport(id: number, status: string) {
  try {
    await patch(`/api/moderation/reports/${id}`, { status })
    ElMessage.success('处理成功')
    fetchReports()
  } catch {
    ElMessage.error('操作失败')
  }
}

onMounted(() => {
  fetchReports()
})
</script>

<style scoped>
.admin-reports {
  max-width: 1400px;
  margin: 0 auto;
}
.page-header {
  margin-bottom: 20px;
}
.page-header h2 {
  margin: 0;
  font-size: 22px;
  font-weight: 600;
}
.filter-card {
  margin-bottom: 16px;
}
.filter-card :deep(.el-card__body) {
  padding: 16px 20px 0;
}
.handled-text {
  font-size: 12px;
  color: #999;
}
.pagination-wrap {
  display: flex;
  justify-content: flex-end;
  margin-top: 16px;
}
</style>
