<template>
  <div class="admin-bans">
    <div class="page-header">
      <h2>封禁管理</h2>
      <el-button type="danger" @click="openCreate">
        <el-icon><Plus /></el-icon> 封禁用户
      </el-button>
    </div>

    <el-card shadow="never" class="filter-card">
      <el-form :inline="true" @submit.prevent="handleSearch">
        <el-form-item label="状态">
          <el-select v-model="statusFilter" placeholder="全部" clearable @change="handleSearch">
            <el-option label="全部" value="" />
            <el-option label="生效中" value="active" />
            <el-option label="已过期" value="expired" />
          </el-select>
        </el-form-item>
        <el-form-item label="搜索">
          <el-input
            v-model="searchQuery"
            placeholder="搜索用户名..."
            clearable
            @clear="handleSearch"
            @keyup.enter="handleSearch"
          >
            <template #prefix><el-icon><Search /></el-icon></template>
          </el-input>
        </el-form-item>
        <el-form-item>
          <el-button type="primary" @click="handleSearch">搜索</el-button>
          <el-button @click="resetFilters">重置</el-button>
        </el-form-item>
      </el-form>
    </el-card>

    <el-card shadow="never">
      <el-table v-loading="loading" :data="bans" border stripe style="width: 100%">
        <el-table-column label="用户" min-width="160">
          <template #default="{ row }">
            <div class="user-cell">
              <el-avatar :size="32" :src="row.target_avatar || undefined">
                {{ (row.target_username || '?')[0].toUpperCase() }}
              </el-avatar>
              <NuxtLink :to="'/user/' + row.target_username" class="user-link">
                {{ row.target_username }}
              </NuxtLink>
            </div>
          </template>
        </el-table-column>

        <el-table-column label="类型" width="100" align="center">
          <template #default="{ row }">
            <el-tag :type="row.type === 'ban' ? 'danger' : 'warning'" size="small">
              {{ row.type === 'ban' ? '封禁' : row.type === 'mute' ? '禁言' : row.type }}
            </el-tag>
          </template>
        </el-table-column>

        <el-table-column label="原因" min-width="200" show-overflow-tooltip>
          <template #default="{ row }">{{ row.reason }}</template>
        </el-table-column>

        <el-table-column label="操作员" width="120">
          <template #default="{ row }">{{ row.operator_name || '系统' }}</template>
        </el-table-column>

        <el-table-column label="创建时间" width="170" sortable>
          <template #default="{ row }">{{ formatDate(row.created_at) }}</template>
        </el-table-column>

        <el-table-column label="过期时间" width="170">
          <template #default="{ row }">
            {{ row.expires_at ? formatDate(row.expires_at) : '永久' }}
          </template>
        </el-table-column>

        <el-table-column label="状态" width="100" align="center">
          <template #default="{ row }">
            <el-tag :type="isActiveBan(row) ? 'success' : 'info'" size="small">
              {{ isActiveBan(row) ? '生效中' : '已失效' }}
            </el-tag>
          </template>
        </el-table-column>

        <el-table-column label="操作" width="220" fixed="right" align="center">
          <template #default="{ row }">
            <el-button
              v-if="isActiveBan(row)"
              type="warning"
              size="small"
              plain
              @click="handleUnban(row)"
            >
              解封
            </el-button>
            <el-button type="primary" size="small" plain @click="openEdit(row)">
              编辑
            </el-button>
            <el-button type="danger" size="small" plain @click="handleDelete(row)">
              删除
            </el-button>
          </template>
        </el-table-column>

        <template #empty>
          <el-empty description="暂无封禁记录" />
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
          @current-change="fetchBans"
          @size-change="handleSizeChange"
        />
      </div>
    </el-card>

    <el-dialog
      v-model="dialogVisible"
      :title="editingBan ? '编辑封禁' : '新建封禁'"
      width="500px"
      :close-on-click-modal="false"
      destroy-on-close
    >
      <el-form ref="formRef" :model="form" :rules="rules" label-width="80px">
        <el-form-item label="用户" prop="userId">
          <el-input
            v-model="form.userId"
            :disabled="!!editingBan"
            placeholder="输入用户名或用户ID"
          />
        </el-form-item>
        <el-form-item label="类型" prop="type">
          <el-select v-model="form.type" :disabled="!!editingBan" style="width: 100%">
            <el-option label="封禁 (无法登录/访问)" value="ban" />
            <el-option label="禁言 (可浏览但不可评论/弹幕)" value="mute" />
          </el-select>
        </el-form-item>
        <el-form-item label="原因" prop="reason">
          <el-input
            v-model="form.reason"
            type="textarea"
            :rows="3"
            placeholder="输入封禁原因"
          />
        </el-form-item>
        <el-form-item label="时长">
          <el-input-number
            v-model="form.duration"
            :min="1"
            :placeholder="'留空为永久'"
            controls-position="right"
            style="width: 100%"
          />
          <div class="form-tip">单位：分钟，留空为永久封禁</div>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="dialogVisible = false">取消</el-button>
        <el-button type="danger" :loading="submitting" @click="handleSave">
          {{ editingBan ? '更新' : '封禁' }}
        </el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { Plus, Search } from '@element-plus/icons-vue'
import { ElMessage } from 'element-plus'
import type { FormInstance, FormRules } from 'element-plus'
import type { UserBan } from '~/types'

definePageMeta({ layout: 'admin', middleware: 'admin-auth' })

const { get, post, put, del } = useRequest()
const { confirmDelete, formatDate } = useAdmin()

const bans = ref<UserBan[]>([])
const total = ref(0)
const page = ref(1)
const size = ref(20)
const loading = ref(false)
const statusFilter = ref('')
const searchQuery = ref('')

const dialogVisible = ref(false)
const editingBan = ref<UserBan | null>(null)
const submitting = ref(false)
const formRef = ref<FormInstance>()

const form = reactive({
  userId: '',
  type: 'ban',
  reason: '',
  duration: null as number | null,
})

const rules: FormRules = {
  userId: [{ required: true, message: '请输入用户ID', trigger: 'blur' }],
  type: [{ required: true, message: '请选择类型', trigger: 'change' }],
  reason: [{ required: true, message: '请输入封禁原因', trigger: 'blur' }],
}

function isActiveBan(ban: UserBan) {
  return ban.is_active && (!ban.expires_at || new Date(ban.expires_at) > new Date())
}

async function fetchBans() {
  loading.value = true
  try {
    const res = await get('/api/admin/bans', {
      page: page.value,
      size: size.value,
      status: statusFilter.value || undefined,
      search: searchQuery.value || undefined,
    })
    if (res.success) {
      bans.value = res.bans || []
      total.value = res.total || 0
    }
  } catch {
    ElMessage.error('加载封禁列表失败')
  } finally {
    loading.value = false
  }
}

function handleSearch() {
  page.value = 1
  fetchBans()
}

function resetFilters() {
  statusFilter.value = ''
  searchQuery.value = ''
  page.value = 1
  fetchBans()
}

function handleSizeChange() {
  page.value = 1
  fetchBans()
}

function openCreate() {
  editingBan.value = null
  form.userId = ''
  form.type = 'ban'
  form.reason = ''
  form.duration = null
  dialogVisible.value = true
}

function openEdit(ban: UserBan) {
  editingBan.value = ban
  form.userId = String(ban.user_id)
  form.type = ban.type
  form.reason = ban.reason
  form.duration = ban.duration || null
  dialogVisible.value = true
}

async function handleSave() {
  if (!formRef.value) return
  const valid = await formRef.value.validate().catch(() => false)
  if (!valid) return

  submitting.value = true
  try {
    if (editingBan.value) {
      await put(`/api/admin/bans/${editingBan.value.id}`, {
        reason: form.reason,
        duration: form.duration,
      })
      ElMessage.success('封禁已更新')
    } else {
      await post('/api/admin/bans', {
        userId: form.userId,
        type: form.type,
        reason: form.reason,
        duration: form.duration,
      })
      ElMessage.success('封禁成功')
    }
    dialogVisible.value = false
    fetchBans()
  } catch (e: any) {
    ElMessage.error(e.data?.message || '操作失败')
  } finally {
    submitting.value = false
  }
}

async function handleUnban(ban: UserBan) {
  const ok = await confirmDelete('确定要解封该用户吗？')
  if (!ok) return
  try {
    await put(`/api/admin/bans/${ban.id}`, { is_active: false })
    ElMessage.success('已解封')
    fetchBans()
  } catch {
    ElMessage.error('解封失败')
  }
}

async function handleDelete(ban: UserBan) {
  const ok = await confirmDelete('确定要删除这条封禁记录吗？')
  if (!ok) return
  try {
    await del(`/api/admin/bans/${ban.id}`)
    ElMessage.success('已删除')
    fetchBans()
  } catch {
    ElMessage.error('删除失败')
  }
}

onMounted(() => {
  fetchBans()
})
</script>

<style scoped>
.admin-bans {
  max-width: 1400px;
  margin: 0 auto;
}
.page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
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
.user-cell {
  display: flex;
  align-items: center;
  gap: 8px;
}
.user-link {
  color: #6c5ce7;
  text-decoration: none;
  font-weight: 500;
}
.user-link:hover {
  text-decoration: underline;
}
.pagination-wrap {
  display: flex;
  justify-content: flex-end;
  margin-top: 16px;
}
.form-tip {
  font-size: 12px;
  color: #999;
  margin-top: 4px;
}
</style>
