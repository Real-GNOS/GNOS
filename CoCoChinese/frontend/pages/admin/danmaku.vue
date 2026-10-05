<template>
  <div class="admin-danmaku">
    <div class="page-header">
      <h2>弹幕审核中心</h2>
    </div>

    <el-card shadow="never" class="filter-card">
      <el-form :inline="true" @submit.prevent="fetchData">
        <el-form-item label="搜索">
          <el-input
            v-model="search"
            placeholder="搜索弹幕内容/用户名"
            clearable
            @clear="fetchData"
            @keyup.enter="fetchData"
          >
            <template #prefix><el-icon><Search /></el-icon></template>
          </el-input>
        </el-form-item>
        <el-form-item label="视频">
          <el-select
            v-model="videoFilter"
            filterable
            remote
            :remote-method="searchVideo"
            placeholder="筛选视频"
            clearable
            @change="fetchData"
          >
            <el-option
              v-for="v in videoOptions"
              :key="v.slug"
              :label="v.title"
              :value="v.slug"
            />
          </el-select>
        </el-form-item>
        <el-form-item>
          <el-button type="primary" @click="fetchData">
            <el-icon><Search /></el-icon> 搜索
          </el-button>
          <el-button @click="resetFilters">
            <el-icon><RefreshRight /></el-icon> 重置
          </el-button>
          <el-button
            type="danger"
            plain
            :disabled="!selected.length"
            @click="batchDelete"
          >
            <el-icon><Delete /></el-icon> 批量删除 ({{ selected.length }})
          </el-button>
        </el-form-item>
      </el-form>
    </el-card>

    <el-card shadow="never">
      <el-table
        v-loading="loading"
        :data="danmaku"
        stripe
        border
        max-height="650"
        style="width: 100%"
        @selection-change="onSelectionChange"
      >
        <el-table-column type="selection" width="40" />

        <el-table-column prop="id" label="ID" width="60" align="center" />

        <el-table-column label="视频" min-width="160" show-overflow-tooltip>
          <template #default="{ row }">
            <span class="video-title">{{ row.video_title || row.video_slug }}</span>
          </template>
        </el-table-column>

        <el-table-column label="用户名" width="120">
          <template #default="{ row }">
            <span :style="{ color: row.color }">{{ row.username }}</span>
          </template>
        </el-table-column>

        <el-table-column label="内容" min-width="300">
          <template #default="{ row }">
            <span
              class="danmaku-content"
              :style="{
                color: row.color,
                textShadow: row.color !== '#ffffff' ? `0 0 4px ${row.color}40` : 'none',
              }"
            >
              {{ row.content }}
            </span>
          </template>
        </el-table-column>

        <el-table-column label="时间点" width="100" align="center">
          <template #default="{ row }">{{ formatTime(row.time) }}</template>
        </el-table-column>

        <el-table-column label="类型" width="80" align="center">
          <template #default="{ row }">
            <el-tag
              :type="row.type === 'scroll' ? 'primary' : row.type === 'top' ? 'warning' : 'info'"
              size="small"
              effect="plain"
            >
              {{ row.type === 'scroll' ? '滚动' : row.type === 'top' ? '顶部' : '底部' }}
            </el-tag>
          </template>
        </el-table-column>

        <el-table-column label="时间" width="160">
          <template #default="{ row }">{{ timeAgo(row.created_at) }}</template>
        </el-table-column>

        <el-table-column label="操作" width="180" fixed="right" align="center">
          <template #default="{ row }">
            <el-button size="small" type="primary" plain @click="addBlockWord(row.content)">
              屏蔽
            </el-button>
            <el-button size="small" type="danger" plain @click="deleteOne(row)">
              删除
            </el-button>
          </template>
        </el-table-column>

        <template #empty>
          <el-empty description="暂无弹幕" />
        </template>
      </el-table>

      <div class="pagination-wrap">
        <el-pagination
          v-model:current-page="page"
          :page-size="size"
          :total="total"
          layout="total, prev, pager, next"
          background
          @current-change="fetchData"
        />
      </div>
    </el-card>

    <el-dialog
      v-model="blockWordDialog"
      title="添加屏蔽词"
      width="420px"
      :close-on-click-modal="false"
      destroy-on-close
    >
      <el-input
        v-model="newBlockWord"
        placeholder="输入要屏蔽的关键词"
        @keyup.enter="saveBlockWord"
      >
        <template #prefix>
          <el-icon style="color: #f56c6c"><CircleClose /></el-icon>
        </template>
      </el-input>
      <div v-if="blockWords.length" class="block-words-list">
        <el-tag
          v-for="(w, i) in blockWords"
          :key="i"
          closable
          type="danger"
          effect="plain"
          @close="removeBlockWord(i)"
        >
          {{ w }}
        </el-tag>
      </div>
      <template #footer>
        <el-button @click="blockWordDialog = false">关闭</el-button>
        <el-button type="primary" @click="saveBlockWord">
          <el-icon><Plus /></el-icon> 添加
        </el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { Search, RefreshRight, Delete, CircleClose, Plus } from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'
import type { Danmaku } from '~/types'

definePageMeta({ layout: 'admin', middleware: 'admin-auth' })

const { get, del, post } = useRequest()
const { confirmDelete, timeAgo } = useAdmin()

const danmaku = ref<Danmaku[]>([])
const total = ref(0)
const page = ref(1)
const size = ref(50)
const loading = ref(false)
const search = ref('')
const videoFilter = ref('')
const videoOptions = ref<any[]>([])
const selected = ref<Danmaku[]>([])

const blockWordDialog = ref(false)
const newBlockWord = ref('')
const blockWords = ref<string[]>([])

function onSelectionChange(val: Danmaku[]) {
  selected.value = val
}

function formatTime(seconds: number) {
  const m = Math.floor(seconds / 60)
  const s = Math.floor(seconds % 60)
  return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`
}

async function fetchData() {
  loading.value = true
  try {
    const res = await get('/api/admin/danmaku', {
      page: page.value,
      size: size.value,
      search: search.value,
      video_slug: videoFilter.value || undefined,
    })
    if (res.success) {
      danmaku.value = res.danmaku || []
      total.value = res.total || 0
    }
  } catch {
    ElMessage.error('加载弹幕失败')
  } finally {
    loading.value = false
  }
}

function resetFilters() {
  search.value = ''
  videoFilter.value = ''
  page.value = 1
  fetchData()
}

async function deleteOne(row: Danmaku) {
  const ok = await confirmDelete('真的要消除这条弹幕吗？')
  if (!ok) return
  try {
    await del(`/api/admin/danmaku/${row.id}`)
    ElMessage.success('弹幕已消除')
    fetchData()
  } catch {
    ElMessage.error('删除失败')
  }
}

async function batchDelete() {
  if (!selected.value.length) return
  const ok = await confirmDelete(`真的要消除选中的 ${selected.value.length} 条弹幕吗？`)
  if (!ok) return
  try {
    const ids = selected.value.map((d) => d.id)
    await post('/api/admin/danmaku/delete-batch', { ids })
    ElMessage.success(`已消除 ${ids.length} 条弹幕`)
    selected.value = []
    fetchData()
  } catch {
    ElMessage.error('批量删除失败')
  }
}

async function searchVideo(query: string) {
  if (!query) return
  try {
    const res = await get('/api/admin/videos', { search: query, size: 10 })
    if (res.success) videoOptions.value = res.videos || []
  } catch {}
}

async function loadBlockWords() {
  try {
    const res = await get('/api/admin/danmaku/block-words')
    if (res.success) blockWords.value = res.blockWords || []
  } catch {}
}

function addBlockWord(content: string) {
  const words = content.match(/[\u4e00-\u9fa5a-zA-Z0-9]+/g) || []
  newBlockWord.value = words.length > 0 ? words[0] : content.slice(0, 20)
  blockWordDialog.value = true
  loadBlockWords()
}

async function saveBlockWord() {
  if (!newBlockWord.value.trim()) return
  try {
    const res = await post('/api/admin/danmaku/block-words', { word: newBlockWord.value.trim() })
    if (res.success) {
      ElMessage.success('屏蔽词已添加')
      blockWords.value = res.blockWords || blockWords.value
      newBlockWord.value = ''
    }
  } catch {
    ElMessage.error('添加失败')
  }
}

async function removeBlockWord(index: number) {
  const word = blockWords.value[index]
  try {
    await $fetch(`/api/admin/danmaku/block-words?word=${encodeURIComponent(word)}`, { method: 'DELETE' })
    blockWords.value.splice(index, 1)
    ElMessage.success('屏蔽词已移除')
  } catch {
    ElMessage.error('移除失败')
  }
}

onMounted(() => {
  fetchData()
})
</script>

<style scoped>
.admin-danmaku {
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
.video-title {
  font-size: 13px;
  font-weight: 500;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.danmaku-content {
  font-size: 14px;
  font-weight: 500;
}
.pagination-wrap {
  display: flex;
  justify-content: flex-end;
  margin-top: 16px;
}
.block-words-list {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 16px;
}
</style>
