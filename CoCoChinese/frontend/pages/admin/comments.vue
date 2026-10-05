<template>
  <div class="admin-comments">
    <div class="page-header">
      <h2>评论管理</h2>
    </div>

    <el-card shadow="never" class="search-card">
      <el-form :inline="true" @submit.prevent="handleSearch">
        <el-form-item>
          <el-input
            v-model="searchQuery"
            placeholder="搜索评论内容/用户名..."
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
        :data="comments"
        border
        stripe
        style="width: 100%"
      >
        <el-table-column prop="id" label="ID" width="80" sortable />
        <el-table-column prop="username" label="用户" width="130" show-overflow-tooltip />
        <el-table-column label="视频" min-width="180">
          <template #default="{ row }">
            <NuxtLink :to="'/player/' + row.video_slug" target="_blank" class="video-link">
              {{ row.video_title || row.video_slug }}
            </NuxtLink>
          </template>
        </el-table-column>
        <el-table-column prop="content" label="内容" min-width="280" show-overflow-tooltip />
        <el-table-column label="时间" width="180" sortable>
          <template #default="{ row }">
            {{ formatDate(row.created_at) }}
          </template>
        </el-table-column>
        <el-table-column label="操作" width="100" fixed="right">
          <template #default="{ row }">
            <el-button type="danger" link size="small" @click="deleteComment(row)">
              删除
            </el-button>
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
          @current-change="fetchComments"
          @size-change="handleSizeChange"
        />
      </div>
    </el-card>
  </div>
</template>

<script setup lang="ts">
import { Search } from '@element-plus/icons-vue'
import { ElMessage, ElMessageBox } from 'element-plus'

definePageMeta({ layout: 'admin', middleware: 'admin-auth' })

interface Comment {
  id: number
  username: string
  video_slug: string
  video_title: string
  content: string
  created_at: string
}

const comments = ref<Comment[]>([])
const total = ref(0)
const page = ref(1)
const size = ref(20)
const searchQuery = ref('')
const loading = ref(false)

async function fetchComments() {
  loading.value = true
  try {
    const res = await $fetch<{ success: boolean; comments: Comment[]; total: number }>('/api/admin/comments', {
      params: { page: page.value, size: size.value, search: searchQuery.value }
    })
    if (res.success) {
      comments.value = res.comments
      total.value = res.total
    }
  } catch {
    ElMessage.error('加载评论列表失败')
  } finally {
    loading.value = false
  }
}

function handleSearch() {
  page.value = 1
  fetchComments()
}

function clearSearch() {
  searchQuery.value = ''
  page.value = 1
  fetchComments()
}

function handleSizeChange() {
  page.value = 1
  fetchComments()
}

async function deleteComment(c: Comment) {
  try {
    await ElMessageBox.confirm(`确定删除评论 #${c.id} 吗？此操作不可恢复。`, '删除确认', {
      confirmButtonText: '确定删除',
      cancelButtonText: '取消',
      type: 'warning'
    })
    await $fetch(`/api/admin/comments/${c.id}`, { method: 'DELETE' })
    ElMessage.success('已删除')
    fetchComments()
  } catch {
    ElMessage.error('删除失败')
  }
}

function formatDate(d: string) {
  return new Date(d).toLocaleString()
}

onMounted(() => {
  fetchComments()
})
</script>

<style scoped>
.admin-comments {
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
.video-link {
  color: var(--el-color-primary);
  text-decoration: none;
}
.video-link:hover {
  text-decoration: underline;
}
</style>
