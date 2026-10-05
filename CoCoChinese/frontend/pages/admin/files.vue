<template>
  <div class="files-page">
    <div class="page-header">
      <h2>文件管理</h2>
    </div>

    <el-row :gutter="16" class="stats-row">
      <el-col :span="12">
        <el-card shadow="never" class="stat-card">
          <div class="stat-icon" style="background: rgba(179, 136, 255, 0.1); color: #b388ff">
            <el-icon :size="24"><Coin /></el-icon>
          </div>
          <div class="stat-info">
            <span class="stat-label">总存储</span>
            <span class="stat-value">{{ formatBytes(totalStorage) }}</span>
          </div>
        </el-card>
      </el-col>
      <el-col :span="12">
        <el-card shadow="never" class="stat-card">
          <div class="stat-icon" style="background: rgba(105, 240, 174, 0.1); color: #69f0ae">
            <el-icon :size="24"><Document /></el-icon>
          </div>
          <div class="stat-info">
            <span class="stat-label">文件总数</span>
            <span class="stat-value">{{ total }}</span>
          </div>
        </el-card>
      </el-col>
    </el-row>

    <el-card shadow="never" class="filter-card">
      <el-row :gutter="12" align="middle">
        <el-col :span="8">
          <el-input
            v-model="search"
            placeholder="搜索文件名/路径"
            clearable
            @clear="fetchData"
            @keyup.enter="fetchData"
          >
            <template #prefix>
              <el-icon><Search /></el-icon>
            </template>
          </el-input>
        </el-col>
        <el-col :span="6">
          <el-select v-model="typeFilter" placeholder="文件类型" clearable @change="fetchData">
            <el-option label="图片" value="image" />
            <el-option label="视频" value="video" />
            <el-option label="音频" value="audio" />
            <el-option label="文档" value="application" />
          </el-select>
        </el-col>
        <el-col :span="10">
          <div class="filter-actions">
            <el-button type="primary" @click="fetchData">
              <el-icon><Search /></el-icon>
              搜索
            </el-button>
            <el-button @click="resetFilters">
              <el-icon><RefreshRight /></el-icon>
              重置
            </el-button>
          </div>
        </el-col>
      </el-row>
    </el-card>

    <el-card shadow="never" class="table-card">
      <el-table :data="files" v-loading="loading" stripe max-height="600">
        <el-table-column prop="id" label="ID" width="60" />
        <el-table-column label="文件名" min-width="200">
          <template #default="{ row }">
            <div class="file-name-cell">
              <span class="file-name">{{ row.name || row.file_path.split('/').pop() }}</span>
            </div>
          </template>
        </el-table-column>
        <el-table-column prop="file_type" label="类型" width="100">
          <template #default="{ row }">
            <el-tag :type="getFileTagType(row.file_type)" size="small" effect="plain">
              {{ getFileLabel(row.file_type) }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column label="大小" width="100">
          <template #default="{ row }">{{ formatBytes(row.file_size) }}</template>
        </el-table-column>
        <el-table-column label="路径" min-width="250">
          <template #default="{ row }">
            <el-tooltip :content="row.file_path" placement="top">
              <code class="path-text">{{ truncatePath(row.file_path) }}</code>
            </el-tooltip>
          </template>
        </el-table-column>
        <el-table-column label="上传时间" width="160">
          <template #default="{ row }">{{ timeAgo(row.created_at) }}</template>
        </el-table-column>
        <el-table-column label="操作" width="120" fixed="right">
          <template #default="{ row }">
            <el-button size="small" type="danger" plain @click="handleDelete(row)">删除</el-button>
          </template>
        </el-table-column>
      </el-table>

      <div class="pagination-wrap">
        <el-pagination
          v-model:current-page="page"
          :page-size="size"
          :total="total"
          layout="prev, pager, next, total"
          @current-change="fetchData"
        />
      </div>
    </el-card>
  </div>
</template>

<script setup lang="ts">
import { Search, RefreshRight, Coin, Document } from '@element-plus/icons-vue'
import type { MediaFile } from '~/types'

definePageMeta({ layout: 'admin', middleware: 'admin-auth' })

const { get, del } = useRequest()
const { confirmDelete, timeAgo, formatBytes, success, error: showError } = useAdmin()

const files = ref<MediaFile[]>([])
const total = ref(0)
const totalStorage = ref(0)
const page = ref(1)
const size = ref(20)
const loading = ref(false)
const search = ref('')
const typeFilter = ref('')

function getFileTagType(type: string | null): 'success' | 'primary' | 'warning' | 'info' {
  if (!type) return 'info'
  if (type.startsWith('image')) return 'success'
  if (type.startsWith('video')) return 'primary'
  if (type.startsWith('audio')) return 'warning'
  return 'info'
}

function getFileLabel(type: string | null): string {
  if (!type) return '未知'
  if (type.startsWith('image')) return '图片'
  if (type.startsWith('video')) return '视频'
  if (type.startsWith('audio')) return '音频'
  if (type.startsWith('application')) return '文档'
  return type
}

function truncatePath(p: string): string {
  if (!p) return '-'
  return p.length > 40 ? '...' + p.slice(-37) : p
}

function resetFilters() {
  search.value = ''
  typeFilter.value = ''
  page.value = 1
  fetchData()
}

async function handleDelete(row: MediaFile) {
  const ok = await confirmDelete('真的要消除这个文件吗？文件将被永久删除！')
  if (!ok) return
  try {
    await del(`/api/admin/files/${row.id}`)
    success('已消除')
    fetchData()
  } catch {
    showError('删除失败')
  }
}

async function fetchData() {
  loading.value = true
  try {
    const res = await get('/api/admin/files', {
      page: page.value,
      size: size.value,
      search: search.value,
      type: typeFilter.value || undefined,
    })
    if (res.success) {
      files.value = res.files || []
      total.value = res.total || 0
      totalStorage.value = res.totalStorage || 0
    }
  } catch {
    showError('加载失败')
  } finally {
    loading.value = false
  }
}

onMounted(fetchData)
</script>

<style scoped>
.files-page {
  padding: 24px;
}

.page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 20px;
}

.page-header h2 {
  margin: 0;
  font-size: 20px;
  font-weight: 600;
}

.stats-row {
  margin-bottom: 16px;
}

.stat-card :deep(.el-card__body) {
  display: flex;
  align-items: center;
  gap: 16px;
  padding: 20px;
}

.stat-icon {
  width: 56px;
  height: 56px;
  border-radius: 12px;
  display: flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
}

.stat-info {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.stat-label {
  font-size: 13px;
  color: var(--el-text-color-secondary);
}

.stat-value {
  font-size: 22px;
  font-weight: 700;
  color: var(--el-text-color-primary);
}

.filter-card {
  margin-bottom: 16px;
}

.filter-actions {
  display: flex;
  gap: 8px;
}

.table-card :deep(.el-card__body) {
  padding: 0;
}

.pagination-wrap {
  display: flex;
  justify-content: center;
  padding: 16px 0;
}

.file-name-cell {
  display: flex;
  align-items: center;
}

.file-name {
  font-weight: 500;
  font-size: 13px;
}

.path-text {
  font-size: 12px;
  cursor: help;
}
</style>
