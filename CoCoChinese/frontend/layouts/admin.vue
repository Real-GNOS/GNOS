<template>
  <div class="admin-app">
    <el-container class="admin-container">
      <el-aside :width="collapsed ? '64px' : '220px'" class="admin-aside">
        <div class="aside-logo" @click="navigateTo('/admin/dashboard')">
          <img src="/images/little_logo.png" alt="" />
          <span v-show="!collapsed">Cocokalo</span>
        </div>
        <el-scrollbar>
          <el-menu
            :default-active="route.path"
            :collapse="collapsed"
            router
            background-color="#1d1e2c"
            text-color="#a3a6b4"
            active-text-color="#6c5ce7"
            class="aside-menu"
          >
            <el-menu-item index="/admin/dashboard">
              <el-icon><DataBoard /></el-icon>
              <template #title>仪表盘</template>
            </el-menu-item>

            <el-sub-menu index="content">
              <template #title>
                <el-icon><VideoCamera /></el-icon>
                <span>内容管理</span>
              </template>
              <el-menu-item index="/admin/videos">
                <el-icon><Film /></el-icon>视频管理
              </el-menu-item>
              <el-menu-item index="/admin/comments">
                <el-icon><ChatDotRound /></el-icon>评论管理
              </el-menu-item>
              <el-menu-item index="/admin/danmaku">
                <el-icon><ChatLineSquare /></el-icon>弹幕审核
              </el-menu-item>
              <el-menu-item index="/admin/carousel">
                <el-icon><Picture /></el-icon>轮播图
              </el-menu-item>
            </el-sub-menu>

            <el-sub-menu index="users">
              <template #title>
                <el-icon><User /></el-icon>
                <span>用户管理</span>
              </template>
              <el-menu-item index="/admin/users">
                <el-icon><UserFilled /></el-icon>用户列表
              </el-menu-item>
              <el-menu-item index="/admin/bans">
                <el-icon><CircleClose /></el-icon>封禁管理
              </el-menu-item>
              <el-menu-item index="/admin/reports">
                <el-icon><Warning /></el-icon>举报处理
              </el-menu-item>
            </el-sub-menu>

            <el-sub-menu index="system">
              <template #title>
                <el-icon><Setting /></el-icon>
                <span>系统管理</span>
              </template>
              <el-menu-item index="/admin/files">
                <el-icon><FolderOpened /></el-icon>文件管理
              </el-menu-item>
              <el-menu-item index="/admin/ai">
                <el-icon><MagicStick /></el-icon>AI 审核
              </el-menu-item>
              <el-menu-item index="/admin/config">
                <el-icon><Tools /></el-icon>系统设置
              </el-menu-item>
              <el-menu-item index="/admin/notices">
                <el-icon><Bell /></el-icon>公告管理
              </el-menu-item>
              <el-menu-item index="/admin/audit">
                <el-icon><Document /></el-icon>操作日志
              </el-menu-item>
            </el-sub-menu>
          </el-menu>
        </el-scrollbar>
      </el-aside>

      <el-container class="admin-main">
        <el-header class="admin-header">
          <div class="header-left">
            <el-icon class="collapse-btn" @click="collapsed = !collapsed">
              <Fold v-if="!collapsed" /><Expand v-else />
            </el-icon>
            <el-breadcrumb separator="/">
              <el-breadcrumb-item :to="{ path: '/admin/dashboard' }">管理后台</el-breadcrumb-item>
              <el-breadcrumb-item v-if="currentPageTitle">{{ currentPageTitle }}</el-breadcrumb-item>
            </el-breadcrumb>
          </div>
          <div class="header-right">
            <el-badge :value="unreadCount" :hidden="unreadCount === 0" :max="99">
              <el-button :icon="Bell" circle size="small" @click="navigateTo('/notifications')" />
            </el-badge>
            <el-dropdown trigger="click" @command="handleUserCommand">
              <span class="user-info">
                <el-avatar :size="30" :src="user?.avatar_url || '/images/authorImg.webp'" />
                <span class="user-name">{{ user?.display_name || user?.username || '管理员' }}</span>
                <el-icon><ArrowDown /></el-icon>
              </span>
              <template #dropdown>
                <el-dropdown-menu>
                  <el-dropdown-item command="home"><el-icon><Monitor /></el-icon>前台首页</el-dropdown-item>
                  <el-dropdown-item divided command="logout" class="danger-item">
                    <el-icon><SwitchButton /></el-icon>退出登录
                  </el-dropdown-item>
                </el-dropdown-menu>
              </template>
            </el-dropdown>
          </div>
        </el-header>
        <el-main class="admin-content">
          <slot />
        </el-main>
      </el-container>
    </el-container>
  </div>
</template>

<script setup lang="ts">
import {
  DataBoard, VideoCamera, Film, ChatDotRound, ChatLineSquare, Picture,
  User, UserFilled, CircleClose, Warning, Setting, FolderOpened,
  MagicStick, Tools, Document, Fold, Expand, Bell, ArrowDown,
  Monitor, SwitchButton
} from '@element-plus/icons-vue'
import { ElMessageBox } from 'element-plus'

const route = useRoute()
const { user, fetchUser } = useUser()
const collapsed = ref(false)
const unreadCount = ref(0)

const pageMap: Record<string, string> = {
  '/admin/dashboard': '仪表盘',
  '/admin/videos': '视频管理',
  '/admin/comments': '评论管理',
  '/admin/danmaku': '弹幕审核',
  '/admin/carousel': '轮播图管理',
  '/admin/users': '用户列表',
  '/admin/bans': '封禁管理',
  '/admin/reports': '举报处理',
  '/admin/files': '文件管理',
  '/admin/ai': 'AI 审核',
  '/admin/config': '系统设置',
  '/admin/notices': '公告管理',
  '/admin/audit': '操作日志',
}

const currentPageTitle = computed(() => pageMap[route.path] || '')

onMounted(() => {
  fetchUser()
  fetchUnread()
})

async function fetchUnread() {
  try {
    const res: any = await $fetch('/api/user/stats')
    unreadCount.value = res?.data?.unreadMessages || 0
  } catch {}
}

function handleUserCommand(cmd: string) {
  if (cmd === 'home') navigateTo('/')
  if (cmd === 'logout') {
    ElMessageBox.confirm('确定退出管理后台？', '退出登录', {
      confirmButtonText: '确定',
      cancelButtonText: '取消',
      type: 'warning',
    }).then(() => {
      document.cookie = 'cocokalo_user=; Max-Age=0; path=/'
      document.cookie = 'cocokalo_token=; Max-Age=0; path=/'
      navigateTo('/admin/login')
    }).catch(() => {})
  }
}
</script>

<style>
body { margin: 0; }
</style>

<style scoped>
.admin-app { height: 100vh; overflow: hidden; }
.admin-container { height: 100vh; }

.admin-aside {
  background: #1d1e2c;
  transition: width 0.3s;
  overflow: hidden;
}
.admin-aside .el-scrollbar { height: calc(100vh - 56px); }

.aside-logo {
  height: 56px;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 10px;
  cursor: pointer;
  border-bottom: 1px solid rgba(255,255,255,0.06);
}
.aside-logo img { width: 28px; height: 28px; border-radius: 6px; }
.aside-logo span { color: #6c5ce7; font-weight: 700; font-size: 16px; letter-spacing: 1px; }

.aside-menu {
  border-right: none !important;
}
.aside-menu:not(.el-menu--collapse) { width: 220px; }

.admin-main { background: #f0f2f5; }
.admin-header {
  background: #fff;
  display: flex;
  align-items: center;
  justify-content: space-between;
  box-shadow: 0 1px 4px rgba(0,0,0,0.06);
  padding: 0 20px;
  z-index: 5;
}
.header-left { display: flex; align-items: center; gap: 16px; }
.collapse-btn { font-size: 20px; cursor: pointer; color: #666; transition: color 0.2s; }
.collapse-btn:hover { color: #6c5ce7; }

.header-right { display: flex; align-items: center; gap: 16px; }
.user-info {
  display: flex;
  align-items: center;
  gap: 8px;
  cursor: pointer;
  padding: 4px 8px;
  border-radius: 6px;
  transition: background 0.2s;
}
.user-info:hover { background: #f5f5f5; }
.user-name { font-size: 14px; font-weight: 500; color: #333; max-width: 100px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

.danger-item { color: #f56c6c !important; }

.admin-content { padding: 20px; overflow-y: auto; }
</style>
