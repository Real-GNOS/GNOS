<template>
  <div class="admin-videos">
    <div class="page-header">
      <h2>视频管理</h2>
    </div>

    <el-card shadow="never" class="search-card">
      <el-form :inline="true" @submit.prevent="handleSearch">
        <el-form-item>
          <el-input
            v-model="searchQuery"
            placeholder="搜索标题/作者..."
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

    <el-card shadow="never" class="table-card">
      <el-table
        v-loading="loading"
        :data="videos"
        border
        stripe
        style="width: 100%"
      >
        <el-table-column prop="id" label="ID" width="80" sortable />
        <el-table-column label="封面" width="100">
          <template #default="{ row }">
            <el-image
              :src="row.image_url || '/images/videoImg.webp'"
              :preview-src-list="[row.image_url]"
              fit="cover"
              style="width: 80px; height: 45px; border-radius: 4px"
            />
          </template>
        </el-table-column>
        <el-table-column prop="title" label="标题" min-width="200" show-overflow-tooltip />
        <el-table-column label="作者" width="130">
          <template #default="{ row }">
            <NuxtLink :to="'/user/' + row.author_username" class="author-link">
              {{ row.author }}
            </NuxtLink>
          </template>
        </el-table-column>
        <el-table-column prop="category" label="分类" width="120">
          <template #default="{ row }">
            {{ row.category || row.video_type || '-' }}
          </template>
        </el-table-column>
        <el-table-column label="状态" width="90" align="center">
          <template #default="{ row }">
            <el-tag :type="row.is_deleted ? 'danger' : 'success'" size="small">
              {{ row.is_deleted ? '已删除' : '正常' }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column label="创建时间" width="180" sortable>
          <template #default="{ row }">
            {{ formatDate(row.created_at) }}
          </template>
        </el-table-column>
        <el-table-column label="操作" width="140" fixed="right">
          <template #default="{ row }">
            <el-button type="primary" link size="small" @click="editVideo(row)">
              编辑
            </el-button>
            <el-button type="danger" link size="small" @click="confirmDelete(row)">
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
          @current-change="fetchVideos"
          @size-change="handleSizeChange"
        />
      </div>
    </el-card>

    <el-dialog v-model="editVisible" title="编辑视频" width="600px" destroy-on-close>
      <el-form :model="editForm" label-width="80px" label-position="top">
        <el-form-item label="标题">
          <el-input v-model="editForm.title" placeholder="请输入标题" />
        </el-form-item>
        <el-form-item label="简介">
          <el-input v-model="editForm.description" type="textarea" :rows="3" placeholder="请输入简介" />
        </el-form-item>
        <el-row :gutter="16">
          <el-col :span="12">
            <el-form-item label="分类">
              <el-input v-model="editForm.category" placeholder="请输入分类" />
            </el-form-item>
          </el-col>
          <el-col :span="12">
            <el-form-item label="标签">
              <el-input v-model="editForm.tags" placeholder="逗号分隔" />
            </el-form-item>
          </el-col>
        </el-row>
        <el-form-item label="封面 URL">
          <el-input v-model="editForm.image_url" placeholder="请输入封面链接" />
        </el-form-item>
        <el-form-item>
          <el-checkbox v-model="editForm.is_deleted">标记为已删除</el-checkbox>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="editVisible = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="saveVideo">保存</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { Search } from '@element-plus/icons-vue'
import { ElMessage, ElMessageBox } from 'element-plus'

definePageMeta({ layout: 'admin', middleware: 'admin-auth' })

interface Video {
  id: number
  title: string
  author: string
  author_username: string
  category: string
  video_type: string
  image_url: string
  is_deleted: boolean
  created_at: string
  description: string
  tags: string[] | string
}

const videos = ref<Video[]>([])
const total = ref(0)
const page = ref(1)
const size = ref(20)
const searchQuery = ref('')
const loading = ref(false)

const editVisible = ref(false)
const saving = ref(false)
const editForm = reactive({
  id: 0,
  title: '',
  description: '',
  category: '',
  tags: '',
  image_url: '',
  is_deleted: false
})

async function fetchVideos() {
  loading.value = true
  try {
    const res = await $fetch<{ success: boolean; videos: Video[]; total: number }>('/api/admin/videos', {
      params: { page: page.value, size: size.value, search: searchQuery.value }
    })
    if (res.success) {
      videos.value = res.videos
      total.value = res.total
    }
  } catch {
    ElMessage.error('加载视频列表失败')
  } finally {
    loading.value = false
  }
}

function handleSearch() {
  page.value = 1
  fetchVideos()
}

function clearSearch() {
  searchQuery.value = ''
  page.value = 1
  fetchVideos()
}

function handleSizeChange() {
  page.value = 1
  fetchVideos()
}

function editVideo(v: Video) {
  editForm.id = v.id
  editForm.title = v.title
  editForm.description = v.description || ''
  editForm.category = v.category || ''
  editForm.tags = Array.isArray(v.tags) ? v.tags.join(',') : (v.tags || '')
  editForm.image_url = v.image_url || ''
  editForm.is_deleted = v.is_deleted || false
  editVisible.value = true
}

async function saveVideo() {
  if (!editForm.id) return
  saving.value = true
  try {
    await $fetch(`/api/admin/videos/${editForm.id}`, { method: 'PUT', body: editForm })
    ElMessage.success('保存成功')
    editVisible.value = false
    fetchVideos()
  } catch (e: any) {
    ElMessage.error(e.data?.message || '保存失败')
  } finally {
    saving.value = false
  }
}

async function confirmDelete(v: Video) {
  try {
    await ElMessageBox.confirm(`确定删除视频「${v.title}」吗？此操作不可恢复。`, '删除确认', {
      confirmButtonText: '确定删除',
      cancelButtonText: '取消',
      type: 'warning'
    })
    await $fetch(`/api/admin/videos/${v.id}`, { method: 'DELETE' })
    ElMessage.success('已删除')
    fetchVideos()
  } catch {
    ElMessage.error('删除失败')
  }
}

function formatDate(d: string) {
  return new Date(d).toLocaleString()
}

onMounted(() => {
  fetchVideos()
})
</script>

<style scoped>
.admin-videos {
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
.table-card {
  margin-bottom: 20px;
}
.pagination-wrap {
  display: flex;
  justify-content: flex-end;
  margin-top: 16px;
}
.author-link {
  color: var(--el-color-primary);
  text-decoration: none;
}
.author-link:hover {
  text-decoration: underline;
}
</style>
