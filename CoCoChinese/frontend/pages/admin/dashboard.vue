<template>
  <div class="dashboard">
    <el-row :gutter="16" class="stat-row">
      <el-col :xs="12" :sm="6" v-for="card in statCards" :key="card.label">
        <el-card shadow="hover" class="stat-card" :body-style="{ padding: '20px' }">
          <div class="stat-content">
            <div class="stat-info">
              <span class="stat-label">{{ card.label }}</span>
              <span class="stat-value">{{ card.value }}</span>
              <span v-if="card.today !== undefined" class="stat-today">今日 +{{ card.today }}</span>
            </div>
            <div class="stat-icon" :style="{ background: card.color }">
              <el-icon :size="24"><component :is="card.icon" /></el-icon>
            </div>
          </div>
        </el-card>
      </el-col>
    </el-row>

    <el-row :gutter="16">
      <el-col :xs="24" :md="16">
        <el-card shadow="never" class="panel-card">
          <template #header>
            <div class="panel-header">
              <span>用户增长趋势 (近7天)</span>
            </div>
          </template>
          <div class="chart-placeholder">
            <div v-for="(item, i) in dashData.userGrowth" :key="i" class="bar-group">
              <div class="bar" :style="{ height: barHeight(item.count, maxUserGrowth) + 'px' }">
                <span class="bar-val">{{ item.count }}</span>
              </div>
              <span class="bar-label">{{ formatDay(item.day) }}</span>
            </div>
          </div>
        </el-card>
      </el-col>
      <el-col :xs="24" :md="8">
        <el-card shadow="never" class="panel-card">
          <template #header>
            <span>内容分类占比</span>
          </template>
          <div class="category-list">
            <div v-for="cat in dashData.categoryStats" :key="cat.category" class="cat-item">
              <span class="cat-name">{{ cat.category }}</span>
              <el-progress :percentage="catPercent(cat.count)" :stroke-width="8" :color="catColor" />
              <span class="cat-count">{{ cat.count }}</span>
            </div>
          </div>
        </el-card>
      </el-col>
    </el-row>

    <el-row :gutter="16" class="mt-16">
      <el-col :xs="24" :md="12">
        <el-card shadow="never" class="panel-card">
          <template #header>
            <div class="panel-header">
              <span>热门视频 TOP5</span>
              <el-button text type="primary" size="small" @click="navigateTo('/admin/videos')">查看全部</el-button>
            </div>
          </template>
          <el-table :data="dashData.topVideos" stripe size="small">
            <el-table-column type="index" width="40" />
            <el-table-column label="封面" width="60">
              <template #default="{ row }">
                <el-image :src="row.image_url || '/images/videoImg.webp'" fit="cover" style="width:48px;height:28px;border-radius:4px" />
              </template>
            </el-table-column>
            <el-table-column prop="title" show-overflow-tooltip min-width="140" />
            <el-table-column label="播放" width="80" align="right">
              <template #default="{ row }">{{ formatNum(row.watch_volue) }}</template>
            </el-table-column>
            <el-table-column label="点赞" width="80" align="right">
              <template #default="{ row }">{{ formatNum(row.like_volue) }}</template>
            </el-table-column>
          </el-table>
        </el-card>
      </el-col>
      <el-col :xs="24" :md="12">
        <el-card shadow="never" class="panel-card">
          <template #header>
            <div class="panel-header">
              <span>最新用户</span>
              <el-button text type="primary" size="small" @click="navigateTo('/admin/users')">查看全部</el-button>
            </div>
          </template>
          <el-table :data="dashData.recentUsers" stripe size="small">
            <el-table-column label="用户" min-width="140">
              <template #default="{ row }">
                <div style="display:flex;align-items:center;gap:8px">
                  <el-avatar :size="28" :src="row.avatar_url || '/images/authorImg.webp'" />
                  <div>
                    <div style="font-weight:500;font-size:13px">{{ row.display_name || row.username }}</div>
                    <div style="font-size:11px;color:#999">@{{ row.username }}</div>
                  </div>
                </div>
              </template>
            </el-table-column>
            <el-table-column label="角色" width="80">
              <template #default="{ row }">
                <el-tag :type="row.role === 'admin' ? 'danger' : 'info'" size="small">{{ row.role }}</el-tag>
              </template>
            </el-table-column>
            <el-table-column label="注册时间" width="140">
              <template #default="{ row }">{{ formatDate(row.created_at) }}</template>
            </el-table-column>
          </el-table>
        </el-card>
      </el-col>
    </el-row>

    <el-row :gutter="16" class="mt-16">
      <el-col :xs="24" :md="12">
        <el-card shadow="never" class="panel-card">
          <template #header>
            <div class="panel-header">
              <span>最新视频</span>
              <el-button text type="primary" size="small" @click="navigateTo('/admin/videos')">查看全部</el-button>
            </div>
          </template>
          <el-table :data="dashData.recentVideos" stripe size="small">
            <el-table-column label="封面" width="60">
              <template #default="{ row }">
                <el-image :src="row.image_url || '/images/videoImg.webp'" fit="cover" style="width:48px;height:28px;border-radius:4px" />
              </template>
            </el-table-column>
            <el-table-column prop="title" show-overflow-tooltip min-width="140" />
            <el-table-column prop="author" width="100" />
            <el-table-column label="状态" width="80">
              <template #default="{ row }">
                <el-tag :type="row.is_deleted ? 'danger' : 'success'" size="small">{{ row.is_deleted ? '已删' : '正常' }}</el-tag>
              </template>
            </el-table-column>
          </el-table>
        </el-card>
      </el-col>
      <el-col :xs="24" :md="12">
        <el-card shadow="never" class="panel-card">
          <template #header>
            <span>快捷操作</span>
          </template>
          <div class="quick-actions">
            <el-button type="primary" @click="navigateTo('/admin/videos')"><el-icon><VideoCamera /></el-icon>管理视频</el-button>
            <el-button type="success" @click="navigateTo('/admin/users')"><el-icon><User /></el-icon>管理用户</el-button>
            <el-button type="warning" @click="navigateTo('/admin/bans')"><el-icon><CircleClose /></el-icon>封禁管理</el-button>
            <el-button type="info" @click="navigateTo('/admin/config')"><el-icon><Setting /></el-icon>系统设置</el-button>
            <el-button type="danger" plain @click="navigateTo('/admin/ai')"><el-icon><MagicStick /></el-icon>AI 审核</el-button>
          </div>
        </el-card>
      </el-col>
    </el-row>
  </div>
</template>

<script setup lang="ts">
import { VideoCamera, User, CircleClose, Setting, MagicStick } from '@element-plus/icons-vue'

definePageMeta({ layout: 'admin', middleware: 'admin-auth' })

const dashData = ref<any>({
  stats: {}, recentUsers: [], recentVideos: [], topVideos: [],
  categoryStats: [], userGrowth: [], videoGrowth: [],
})

const statCards = computed(() => {
  const s = dashData.value.stats
  return [
    { label: '总用户', value: formatNum(s.totalUsers), today: s.todayNewUsers, icon: 'User', color: 'linear-gradient(135deg, #6c5ce7, #a29bfe)' },
    { label: '总视频', value: formatNum(s.totalVideos), today: s.todayNewVideos, icon: 'VideoCamera', color: 'linear-gradient(135deg, #00b894, #55efc4)' },
    { label: '总评论', value: formatNum(s.totalComments), today: s.todayNewComments, icon: 'ChatDotRound', color: 'linear-gradient(135deg, #fdcb6e, #ffeaa7)' },
    { label: '总播放', value: formatNum(s.totalViews), icon: 'View', color: 'linear-gradient(135deg, #e17055, #fab1a0)' },
  ]
})

const maxUserGrowth = computed(() => Math.max(1, ...dashData.value.userGrowth.map((i: any) => i.count)))
const totalCatCount = computed(() => dashData.value.categoryStats.reduce((a: number, c: any) => a + parseInt(c.count), 0) || 1)

function catPercent(count: string) { return Math.round((parseInt(count) / totalCatCount.value) * 100) }
function barHeight(count: number, max: number) { return Math.max(4, (count / max) * 120) }
function formatDay(d: string) { return d?.slice(5) || '' }
function formatNum(n: any) { const v = parseInt(n); if (isNaN(v)) return '0'; if (v >= 10000) return (v / 10000).toFixed(1) + '万'; return String(v) }
function formatDate(d: string) { return new Date(d).toLocaleDateString() }

const catColor = [
  { color: '#6c5ce7', percentage: 100 },
  { color: '#00b894', percentage: 100 },
  { color: '#fdcb6e', percentage: 100 },
  { color: '#e17055', percentage: 100 },
  { color: '#74b9ff', percentage: 100 },
  { color: '#fd79a8', percentage: 100 },
  { color: '#a29bfe', percentage: 100 },
  { color: '#636e72', percentage: 100 },
]

onMounted(async () => {
  try {
    const res: any = await $fetch('/api/admin/dashboard')
    dashData.value = res
  } catch {}
})
</script>

<style scoped>
.dashboard { max-width: 1400px; }
.stat-row { margin-bottom: 16px; }
.stat-card { border-radius: 12px; border: none; }
.stat-content { display: flex; justify-content: space-between; align-items: center; }
.stat-info { display: flex; flex-direction: column; }
.stat-label { font-size: 13px; color: #909399; }
.stat-value { font-size: 28px; font-weight: 700; color: #1d1e2c; margin: 4px 0; }
.stat-today { font-size: 12px; color: #67c23a; }
.stat-icon {
  width: 56px; height: 56px; border-radius: 14px;
  display: flex; align-items: center; justify-content: center; color: #fff;
}
.panel-card { border-radius: 12px; border: none; }
.panel-card :deep(.el-card__header) { padding: 14px 20px; border-bottom: 1px solid #f0f0f0; }
.panel-header { display: flex; justify-content: space-between; align-items: center; font-weight: 600; font-size: 15px; }
.mt-16 { margin-top: 16px; }

.chart-placeholder {
  display: flex;
  align-items: flex-end;
  gap: 8px;
  height: 160px;
  padding: 10px 0;
}
.bar-group { flex: 1; display: flex; flex-direction: column; align-items: center; gap: 4px; }
.bar {
  width: 100%;
  max-width: 40px;
  background: linear-gradient(180deg, #6c5ce7, #a29bfe);
  border-radius: 4px 4px 0 0;
  position: relative;
  min-height: 4px;
  transition: height 0.3s;
}
.bar-val {
  position: absolute;
  top: -18px;
  left: 50%;
  transform: translateX(-50%);
  font-size: 11px;
  font-weight: 600;
  color: #6c5ce7;
}
.bar-label { font-size: 11px; color: #999; }

.category-list { display: flex; flex-direction: column; gap: 12px; }
.cat-item { display: flex; align-items: center; gap: 12px; }
.cat-name { font-size: 13px; width: 80px; flex-shrink: 0; color: #606266; }
.cat-count { font-size: 13px; color: #909399; width: 40px; text-align: right; }

.quick-actions { display: flex; flex-wrap: wrap; gap: 10px; }
.quick-actions .el-button { border-radius: 8px; }
</style>
