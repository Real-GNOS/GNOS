'<template>
  <div class="carousel-page">
    <div class="page-header">
      <h2>轮播图管理</h2>
      <el-button type="primary" @click="openCreate">
        <el-icon><Plus /></el-icon>
        新增轮播图
      </el-button>
    </div>

    <el-card shadow="never" class="table-card">
      <el-table :data="carousels" v-loading="loading" stripe max-height="650">
        <el-table-column prop="id" label="ID" width="60" />
        <el-table-column label="图片" width="160">
          <template #default="{ row }">
            <el-image
              :src="row.image_url || '/images/banner1.jpg'"
              fit="cover"
              style="width: 120px; height: 68px; border-radius: 8px"
              :preview-src-list="[row.image_url || '/images/banner1.jpg']"
            />
          </template>
        </el-table-column>
        <el-table-column prop="title" label="标题" min-width="160" show-overflow-tooltip />
        <el-table-column prop="author" label="作者" width="120" show-overflow-tooltip />
        <el-table-column prop="order" label="排序" width="80" sortable />
        <el-table-column label="链接" min-width="200">
          <template #default="{ row }">
            <el-link
              v-if="row.link"
              :href="row.link"
              target="_blank"
              type="primary"
              :underline="false"
            >
              {{ row.link }}
            </el-link>
            <span v-else class="text-secondary">-</span>
          </template>
        </el-table-column>
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
      :title="isEdit ? '编辑轮播图' : '新增轮播图'"
      width="600px"
      :close-on-click-modal="false"
      destroy-on-close
    >
      <el-form :model="form" label-width="100px" @submit.prevent="save">
        <el-form-item label="图片">
          <el-upload
            class="carousel-uploader"
            action="/api/admin/carousel/upload"
            name="image"
            :show-file-list="false"
            accept="image/*"
            :before-upload="beforeUpload"
            :on-success="onUploadSuccess"
            :on-error="onUploadError"
          >
            <img v-if="form.image_url" :src="form.image_url" class="uploader-preview" />
            <el-icon v-else class="uploader-icon"><Plus /></el-icon>
          </el-upload>
          <div class="uploader-tip">点击上传图片，无需填写链接（支持 png/jpg/webp 等，≤5MB）</div>
        </el-form-item>
        <el-form-item label="标题">
          <el-input v-model="form.title" placeholder="轮播图标题" />
        </el-form-item>
        <el-form-item label="跳转链接">
          <el-input v-model="form.link" placeholder="/video/slug" />
        </el-form-item>
        <el-form-item label="作者">
          <el-input v-model="form.author" placeholder="作者名" />
        </el-form-item>
        <el-form-item label="播放量">
          <el-input-number v-model="form.watch_volue" :min="0" />
        </el-form-item>
        <el-form-item label="点赞数">
          <el-input-number v-model="form.like_volue" :min="0" />
        </el-form-item>
        <el-form-item label="简介">
          <el-input v-model="form.introduction" type="textarea" :rows="3" />
        </el-form-item>
        <el-form-item label="视频时长">
          <el-input v-model="form.video_time" placeholder="例如: 24:00" />
        </el-form-item>
        <el-form-item label="描述">
          <el-input v-model="form.description" type="textarea" :rows="2" />
        </el-form-item>
        <el-form-item label="排序权重">
          <el-input-number v-model="form.order" :min="0" :max="999" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="dialogVisible = false">取消</el-button>
        <el-button type="primary" :loading="saving" @click="save">
          {{ isEdit ? '更新' : '创建' }}
        </el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { Plus } from '@element-plus/icons-vue'
import type { Carousel } from '~/types'

definePageMeta({ layout: 'admin', middleware: 'admin-auth' })

const { get, post, put, del } = useRequest()
const { confirmDelete, timeAgo, success, error: showError } = useAdmin()

const carousels = ref<Carousel[]>([])
const total = ref(0)
const page = ref(1)
const size = ref(20)
const loading = ref(false)
const dialogVisible = ref(false)
const isEdit = ref(false)
const editingId = ref<number | null>(null)
const saving = ref(false)

const form = reactive({
  image_url: '',
  link: '',
  title: '',
  author: '',
  watch_volue: 0,
  like_volue: 0,
  introduction: '',
  video_time: '',
  description: '',
  order: 0,
})

function resetForm() {
  form.image_url = ''
  form.link = ''
  form.title = ''
  form.author = ''
  form.watch_volue = 0
  form.like_volue = 0
  form.introduction = ''
  form.video_time = ''
  form.description = ''
  form.order = 0
}

function openCreate() {
  isEdit.value = false
  editingId.value = null
  resetForm()
  dialogVisible.value = true
}

function beforeUpload(file: any) {
  if (!file.type?.startsWith('image/')) {
    showError('请选择图片文件')
    return false
  }
  if (file.size / 1024 / 1024 > 5) {
    showError('图片大小不能超过 5MB')
    return false
  }
  return true
}

function onUploadSuccess(response: any) {
  if (response?.url) {
    form.image_url = response.url
    success('图片上传成功')
  } else {
    showError('上传失败')
  }
}

function onUploadError() {
  showError('图片上传失败')
}

function openEdit(row: Carousel) {
  isEdit.value = true
  editingId.value = row.id
  Object.assign(form, {
    image_url: row.image_url || '',
    link: row.link || '',
    title: row.title || '',
    author: row.author || '',
    watch_volue: Number(row.watch_volue) || 0,
    like_volue: Number(row.like_volue) || 0,
    introduction: row.introduction || '',
    video_time: row.video_time || '',
    description: row.description || '',
    order: row.order || 0,
  })
  dialogVisible.value = true
}

async function save() {
  
  saving.value = true
  try {
    if (isEdit.value && editingId.value) {
      await put(`/api/admin/carousel/${editingId.value}`, form)
      success('更新成功')
    } else {
      await post('/api/admin/carousel', form)
      success('创建成功')
    }
    dialogVisible.value = false
    fetchData()
  } catch {
    showError('操作失败')
  } finally {
    saving.value = false
  }
}

async function handleDelete(row: Carousel) {
  const ok = await confirmDelete('真的要消除这张轮播图吗？')
  if (!ok) return
  try {
    await del(`/api/admin/carousel/${row.id}`)
    success('已消除')
    fetchData()
  } catch {
    showError('删除失败')
  }
}

async function fetchData() {
  loading.value = true
  try {
    const res = await get('/api/admin/carousel', { page: page.value, size: size.value })
    if (res.success) {
      carousels.value = res.carousels || []
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
.carousel-page {
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

.carousel-uploader :deep(.el-upload) {
  border: 1px dashed var(--el-border-color);
  border-radius: 8px;
  cursor: pointer;
  overflow: hidden;
  width: 240px;
  height: 135px;
  display: flex;
  align-items: center;
  justify-content: center;
  transition: border-color 0.3s;
}

.carousel-uploader :deep(.el-upload):hover {
  border-color: var(--el-color-primary);
}

.uploader-icon {
  font-size: 28px;
  color: var(--el-text-color-placeholder);
}

.uploader-preview {
  width: 240px;
  height: 135px;
  object-fit: cover;
  display: block;
}

.uploader-tip {
  font-size: 12px;
  color: var(--el-text-color-placeholder);
  margin-top: 8px;
}
</style>
