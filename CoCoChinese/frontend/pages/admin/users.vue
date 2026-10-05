<template>
  <div class="admin-users">
    <div class="page-header">
      <h2>用户管理</h2>
    </div>

    <el-card shadow="never" class="search-card">
      <el-form :inline="true" @submit.prevent="handleSearch">
        <el-form-item>
          <el-input
            v-model="searchQuery"
            placeholder="搜索用户名/邮箱..."
            clearable
            @clear="handleSearch"
            @keyup.enter="handleSearch"
          >
            <template #prefix>
              <el-icon><Search /></el-icon>
            </template>
          </el-input>
        </el-form-item>
        <el-form-item>
          <el-button type="primary" @click="handleSearch">搜索</el-button>
          <el-button @click="clearSearch">重置</el-button>
        </el-form-item>
      </el-form>
    </el-card>

    <el-card shadow="never">
      <el-table
        v-loading="loading"
        :data="users"
        border
        stripe
        style="width: 100%"
      >
        <el-table-column label="头像" width="80" align="center">
          <template #default="{ row }">
            <el-avatar :size="36" :src="row.avatar || undefined">
              {{ (row.username || '?')[0].toUpperCase() }}
            </el-avatar>
          </template>
        </el-table-column>
        <el-table-column prop="username" label="用户名" min-width="150" show-overflow-tooltip />
        <el-table-column prop="role" label="角色" width="120" align="center">
          <template #default="{ row }">
            <el-tag :type="row.role === 'admin' ? 'danger' : 'info'" size="small">
              {{ row.role }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="email" label="邮箱" min-width="200" show-overflow-tooltip>
          <template #default="{ row }">
            {{ row.email || '-' }}
          </template>
        </el-table-column>
        <el-table-column label="创建时间" width="180" sortable>
          <template #default="{ row }">
            {{ formatDate(row.created_at || row.createdAt) }}
          </template>
        </el-table-column>
      </el-table>

      <div class="pagination-wrap">
        <el-pagination
          v-model:current-page="page"
          v-model:page-size="size"
          :total="total"
          :page-sizes="[10, 20, 50]"
          layout="total, sizes, prev, pager, next, jumper"
          background
          @current-change="fetchUsers"
          @size-change="handleSizeChange"
        />
      </div>
    </el-card>
  </div>
</template>

<script setup lang="ts">
import { Search } from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'

definePageMeta({ layout: 'admin', middleware: 'admin-auth' })

interface User {
  _id: string
  username: string
  role: string
  email: string
  avatar: string
  created_at: string
  createdAt: string
}

const users = ref<User[]>([])
const total = ref(0)
const page = ref(1)
const size = ref(20)
const searchQuery = ref('')
const loading = ref(false)

async function fetchUsers() {
  loading.value = true
  try {
    const res = await $fetch<{ success: boolean; users: User[]; total: number }>('/api/admin/users', {
      params: { page: page.value, size: size.value, search: searchQuery.value }
    })
    if (res.success) {
      users.value = res.users
      total.value = res.total
    }
  } catch {
    ElMessage.error('加载用户列表失败')
  } finally {
    loading.value = false
  }
}

function handleSearch() {
  page.value = 1
  fetchUsers()
}

function clearSearch() {
  searchQuery.value = ''
  page.value = 1
  fetchUsers()
}

function handleSizeChange() {
  page.value = 1
  fetchUsers()
}

function formatDate(d: string) {
  if (!d) return '-'
  return new Date(d).toLocaleString()
}

onMounted(() => {
  fetchUsers()
})
</script>

<style scoped>
.admin-users {
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
.search-card {
  margin-bottom: 16px;
}
.search-card :deep(.el-card__body) {
  padding: 16px 20px 0;
}
.pagination-wrap {
  display: flex;
  justify-content: flex-end;
  margin-top: 16px;
}
</style>
