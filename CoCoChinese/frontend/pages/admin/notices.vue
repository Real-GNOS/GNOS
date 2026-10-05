<template>
  <div class="notice-page">
    <div class="page-header">
      <h2>公告管理</h2>
      <el-button type="primary" @click="openCreate">
        <el-icon><Plus /></el-icon>
        发布公告
      </el-button>
    </div>

    <el-card shadow="never" class="table-card">
      <el-table :data="notices" v-loading="loading" stripe max-height="650">
        <el-table-column prop="id" label="ID" width="60" />
        <el-table-column prop="title" label="标题" min-width="160" show-overflow-tooltip />
        <el-table-column label="内容" min-width="240" show-overflow-tooltip>
          <template #default="{ row }">{{ row.content || '-' }}</template>
        </el-table-column>
        <el-table-column label="跳转链接" min-width="160" show-overflow-tooltip>
          <template #default="{ row }">
            <el-link v-if="row.link" :href="row.link" target="_blank" type="primary" :underline="false">{{ row.link }}</el-link>
            <span v-else class="text-secondary">-</span>
          </template>
        </el-table-column>
        <el-table-column label="状态" width="90">
          <template #default="{ row }">
            <el-tag :type="row.is_active ? 'success' : 'info'" size="small">
              {{ row.is_active ? '已发布' : '草稿' }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="order" label="排序" width="80" sortable />
        <el-table-column label="创建时间" width="160">
          <template #default="{ row }">{{ timeAgo(row.created_at) }}</template>
        </el-table-column>
        <el-table-column label="操作" width="180" fixed="right">
          <template #default="{ row }">
            <el-button size="small" type="primary" plain @click="openEdit(row)">编辑</el-button>
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

    <el-dialog
      v-model="dialogVisible"
      :title="isEdit ? '编辑公告' : '发布公告'"
      width="600px"
      :close-on-click-modal="false"
      destroy-on-close
    >
      <el-form :model="form" label-width="90px" @submit.prevent="save">
        <el-form-item label="标题">
          <el-input v-model="form.title" placeholder="公告标题" />
        </el-form-item>
        <el-form-item label="正文">
          <el-input v-model="form.content" type="textarea" :rows="4" placeholder="公告正文内容" />
        </el-form-item>
        <el-form-item label="跳转链接">
          <el-input v-model="form.link" placeholder="可选，如 /activity 或 https://..." />
        </el-form-item>
        <el-form-item label="状态">
          <el-switch v-model="form.is_active" active-text="已发布" inactive-text="草稿" />
        </el-form-item>
        <el-form-item label="排序权重">
          <el-input-number v-model="form.order" :min="0" :max="999" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="dialogVisible = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="save">
          {{ isEdit ? '更新' : '发布' }}
        </el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { Plus } from '@element-plus/icons-vue'

definePageMeta({ layout: 'admin', middleware: 'admin-auth' })

const { get, post, put, del } = useRequest()
const { confirmDelete, timeAgo, success, error: showError } = useAdmin()

const notices = ref<any[]>([])
const total = ref(0)
const page = ref(1)
const size = ref(20)
const loading = ref(false)
const dialogVisible = ref(false)
const isEdit = ref(false)
const editingId = ref<number | null>(null)
const saving = ref(false)

const form = reactive({
  title: '',
  content: '',
  link: '',
  is_active: true,
  order: 0,
})

function resetForm() {
  form.title = ''
  form.content = ''
  form.link = ''
  form.is_active = true
  form.order = 0
}

function openCreate() {
  isEdit.value = false
  editingId.value = null
  resetForm()
  dialogVisible.value = true
}

function openEdit(row: any) {
  isEdit.value = true
  editingId.value = row.id
  Object.assign(form, {
    title: row.title || '',
    content: row.content || '',
    link: row.link || '',
    is_active: row.is_active !== false,
    order: row.order || 0,
  })
  dialogVisible.value = true
}

async function save() {
  if (!form.title.trim()) {
    showError('标题不能为空')
    return
  }
  saving.value = true
  try {
    if (isEdit.value && editingId.value) {
      await put(`/api/admin/notices/${editingId.value}`, { ...form })
      success('更新成功')
    } else {
      await post('/api/admin/notices', { ...form })
      success('发布成功')
    }
    dialogVisible.value = false
    fetchData()
  } catch {
    showError('操作失败')
  } finally {
    saving.value = false
  }
}

async function handleDelete(row: any) {
  const ok = await confirmDelete('真的要删除这条公告吗？')
  if (!ok) return
  try {
    await del(`/api/admin/notices/${row.id}`)
    success('已删除')
    fetchData()
  } catch {
    showError('删除失败')
  }
}

async function fetchData() {
  loading.value = true
  try {
    const res = await get('/api/admin/notices', { page: page.value, size: size.value })
    if (res.success) {
      notices.value = res.notices || []
      total.value = res.total || 0
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
.notice-page {
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

.table-card :deep(.el-card__body) {
  padding: 0;
}

.pagination-wrap {
  display: flex;
  justify-content: center;
  padding: 16px 0;
}

.text-secondary {
  color: var(--el-text-color-placeholder);
}
</style>
