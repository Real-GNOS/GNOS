<template>
  <nav class="main-nav container-fluid" :class="{ scrolled: isScrolled, 'nav-hidden': isHidden }">
    <div class="nav-container">
      <button class="mobile-menu-toggle" aria-label="菜单" :aria-expanded="menuOpen" @click="toggleMenu">
        <span class="hamburger-icon"></span>
      </button>

      <div class="nav-left">
        <NuxtLink to="/" class="home-link">
          <img src="/images/little_logo.png" alt="网站Logo" width="30" height="30">
          <span>首页</span>
        </NuxtLink>
        <ul class="nav-links desktop-only">
          <li v-for="item in mainNav.leftList" :key="item.text" class="nav-item" :class="{ 'has-dropdown': item.children }">
            <NuxtLink :to="item.link" :class="item.sty">{{ item.text }}<i v-if="item.children" class="fa fa-angle-down dropdown-arrow" aria-hidden="true"></i></NuxtLink>
            <ul v-if="item.children" class="dropdown-menu-custom">
              <li v-for="child in item.children" :key="child.text" class="dropdown-item-custom">
                <NuxtLink :to="child.link">{{ child.text }}</NuxtLink>
              </li>
            </ul>
          </li>
        </ul>
      </div>

      <div class="search-container">
        <form class="search-form" role="search" @submit.prevent="handleSearch">
          <div class="search-input-wrapper" :class="{ focused: searchFocused }">
            <input v-model="searchQuery" type="search" placeholder="搜索内容..." aria-label="搜索" class="search-input" @focus="searchFocused = true" @blur="searchFocused = false">
            <button type="submit" class="search-button" aria-label="搜索">
              <i class="fa fa-search" aria-hidden="true"></i>
            </button>
          </div>
        </form>
      </div>

      <div class="nav-right">
        <template v-if="me">
          <ul class="nav-links user-links">
            <li class="nav-item nav-bell-item">
              <NuxtLink to="/messages" class="nav-bell-link" title="消息中心">
                <i class="fa fa-bell" aria-hidden="true"></i>
                <span v-if="unreadCount > 0" class="nav-bell-badge">{{ unreadCountDisplay }}</span>
              </NuxtLink>
            </li>
            <li class="nav-item">
              <NuxtLink to="/user/center" class="nav-user-link">
                <img :src="me.avatar_url || '/images/default_avatar.png'" alt="" class="nav-avatar" width="28" height="28">
                <span class="nav-user-name">{{ me.display_name || me.username }}</span>
              </NuxtLink>
            </li>
            <li v-for="item in mainNav.rightList" :key="item.text" class="nav-item">
              <NuxtLink :to="item.link" :class="item.sty">{{ item.text }}</NuxtLink>
            </li>
            <li class="nav-item">
              <a href="#" class="nav-logout" @click.prevent="handleLogout">退出</a>
            </li>
          </ul>
        </template>
        <template v-else>
          <div class="auth-links">
            <NuxtLink to="/inboundNotice" class="notice-link">入站须知</NuxtLink>
            <div class="login-register">
              <NuxtLink to="/login" class="login-button">登录</NuxtLink>
              <span class="divider">/</span>
              <NuxtLink to="/register" class="register-button">注册</NuxtLink>
            </div>
          </div>
        </template>
      </div>
    </div>

    <div class="mobile-menu" :class="{ active: menuOpen }">
      <div class="mobile-search">
        <form class="search-form-mobile" role="search" @submit.prevent="handleSearch">
          <input v-model="searchQuery" type="search" placeholder="搜索内容..." aria-label="搜索" class="search-input-mobile">
          <button type="submit" class="search-button-mobile" aria-label="搜索">
            <i class="fa fa-search" aria-hidden="true"></i>
          </button>
        </form>
      </div>
      <ul class="mobile-nav-links">
        <li v-for="item in mainNav.leftList" :key="item.text" class="mobile-nav-item" :class="{ 'has-children': item.children }">
          <NuxtLink :to="item.link" :class="item.sty" @click="menuOpen = false">{{ item.text }}</NuxtLink>
          <ul v-if="item.children" class="mobile-sub-links">
            <li v-for="child in item.children" :key="child.text" class="mobile-sub-item">
              <NuxtLink :to="child.link" @click="menuOpen = false">{{ child.text }}</NuxtLink>
            </li>
          </ul>
        </li>
      </ul>
      <div class="mobile-auth">
        <template v-if="me">
          <ul class="mobile-user-links">
            <li class="mobile-nav-item">
              <NuxtLink to="/messages" @click="menuOpen = false">
                <i class="fa fa-bell" aria-hidden="true"></i> 消息
                <span v-if="unreadCount > 0" class="mobile-bell-badge">{{ unreadCountDisplay }}</span>
              </NuxtLink>
            </li>
            <li v-for="item in mainNav.rightList" :key="item.text" class="mobile-nav-item">
              <NuxtLink :to="item.link" :class="item.sty" @click="menuOpen = false">{{ item.text }}</NuxtLink>
            </li>
          </ul>
        </template>
        <template v-else>
          <NuxtLink to="/inboundNotice" class="mobile-notice-link" @click="menuOpen = false">入站须知</NuxtLink>
          <div class="mobile-login-register">
            <NuxtLink to="/login" class="mobile-login-button" @click="menuOpen = false">登录</NuxtLink>
            <span class="mobile-divider">/</span>
            <NuxtLink to="/register" class="mobile-register-button" @click="menuOpen = false">注册</NuxtLink>
          </div>
        </template>
      </div>
    </div>
  </nav>
</template>

<script setup lang="ts">
const props = defineProps({ mainNav: { type: Object, default: () => ({ leftList: [], rightList: [], loginFlag: false }) } })

const menuOpen = ref(false)
const searchFocused = ref(false)
const searchQuery = ref('')
const isScrolled = ref(false)
const isHidden = ref(false)
let lastScrollTop = 0

const { user: me, fetchUser } = useUser()
if (!me.value) fetchUser()

const unreadCount = ref(0)
const unreadCountDisplay = computed(() => unreadCount.value > 99 ? '99+' : String(unreadCount.value))
let unreadPollTimer = null as ReturnType<typeof setInterval> | null

async function fetchUnreadCount() {
  if (!me.value) return
  try {
    const res = await $fetch('/api/user/stats')
    if (res?.data?.unreadMessages !== undefined) {
      unreadCount.value = res.data.unreadMessages
    }
  } catch { /* ignore */ }
}

function startUnreadPolling() {
  fetchUnreadCount()
  unreadPollTimer = setInterval(fetchUnreadCount, 30000)
  document.addEventListener('visibilitychange', onVisibilityChange)
}

function stopUnreadPolling() {
  if (unreadPollTimer) { clearInterval(unreadPollTimer); unreadPollTimer = null }
  document.removeEventListener('visibilitychange', onVisibilityChange)
}

function onVisibilityChange() {
  if (document.visibilityState === 'visible') fetchUnreadCount()
}

watch(() => me.value, (val) => {
  if (val && !unreadPollTimer) startUnreadPolling()
  else if (!val) stopUnreadPolling()
})

function toggleMenu() { menuOpen.value = !menuOpen.value }

function handleSearch() {
  if (searchQuery.value.trim()) {
    navigateTo(`/search?q=${encodeURIComponent(searchQuery.value)}`)
  }
}

function handleLogout() {
  document.cookie = 'cocokalo_user=; Max-Age=0; path=/'
  document.cookie = 'cocokalo_token=; Max-Age=0; path=/'
  const redirect = me.value?.role === 'admin' ? '/admin/login' : '/'
  navigateTo(redirect, { external: true })
}

onMounted(() => {
  window.addEventListener('scroll', () => {
    const scrollTop = window.pageYOffset || document.documentElement.scrollTop
    isScrolled.value = scrollTop > 50
    isHidden.value = scrollTop > lastScrollTop && scrollTop > 200
    lastScrollTop = scrollTop
  })
})
</script>

<style scoped>
.main-nav {
  position: fixed; top: 0; left: 0; width: 100%;
  background-color: #fff; box-shadow: 0 2px 10px rgba(0,0,0,0.1);
  z-index: 1000; transition: transform 0.3s ease, background-color 0.3s ease;
}
.nav-container {
  display: flex; align-items: center; justify-content: space-between;
  padding: 0.8rem 1.5rem; max-width: 1300px; margin: 0 auto; overflow: hidden;
}
.main-nav.scrolled { background-color: rgba(255,255,255,0.95); box-shadow: 0 2px 15px rgba(0,0,0,0.15); }
.main-nav.nav-hidden { transform: translateY(-100%); }
.nav-left { display: flex; align-items: center; overflow: hidden; }
.home-link { display: flex; align-items: center; text-decoration: none; color: #333; margin-right: 1rem; flex-shrink: 0; }
.home-link img { margin-right: 0.5rem; }
.nav-links { display: flex; list-style: none; margin: 0; padding: 0; overflow-x: auto; overflow-y: hidden; -webkit-overflow-scrolling: touch; scrollbar-width: thin; }
.nav-item { margin: 0; white-space: nowrap; position: relative; }
.nav-item > a { display: flex; align-items: center; gap: 4px; padding: 0.5rem 1rem; color: #333; text-decoration: none; font-weight: 500; transition: color 0.2s ease, background 0.2s ease; font-size: 0.9rem; border-radius: 6px; }
.nav-item > a:hover { color: #007bff; background: rgba(0,123,255,0.06); }
.nav-item.has-dropdown { padding-right: 0; }
.dropdown-arrow { font-size: 0.7rem; transition: transform 0.2s ease; }
.nav-item.has-dropdown:hover .dropdown-arrow { transform: rotate(180deg); }
.dropdown-menu-custom { display: none; position: absolute; top: 100%; left: 0; min-width: 160px; background: #fff; border: 1px solid #eee; border-radius: 10px; box-shadow: 0 8px 24px rgba(0,0,0,0.12); padding: 0.5rem 0; z-index: 1001; list-style: none; margin: 0; }
.nav-item.has-dropdown:hover .dropdown-menu-custom { display: block; }
.dropdown-item-custom { margin: 0; }
.dropdown-item-custom a { display: block; padding: 0.5rem 1.2rem; color: #333; text-decoration: none; font-size: 0.85rem; transition: background 0.15s ease; white-space: nowrap; }
.dropdown-item-custom a:hover { background: #f5f7fa; color: #007bff; }
.search-container { flex: 0 1 260px; min-width: 140px; }
.search-form { width: 100%; }
.search-input-wrapper { display: flex; border: 1px solid #ddd; border-radius: 20px; overflow: hidden; transition: box-shadow 0.3s ease; }
.search-input-wrapper.focused { box-shadow: 0 0 0 2px rgba(0,123,255,0.25); border-color: #007bff; }
.search-input { flex: 1; border: none; padding: 0.5rem 1rem; outline: none; font-size: 0.9rem; }
.search-button { background: none; border: none; padding: 0.5rem 1rem; cursor: pointer; color: #666; }
.search-button:hover { color: #007bff; }
.nav-right { display: flex; align-items: center; flex-shrink: 0; gap: 0.3rem; }
.nav-right .nav-item > a { padding: 0.5rem 0.8rem; font-size: 0.85rem; }
.auth-links { display: flex; align-items: center; }
.notice-link { margin-right: 1.5rem; color: #666; text-decoration: none; font-size: 0.9rem; }
.login-register { display: flex; align-items: center; }
.login-button, .register-button { color: #333; text-decoration: none; font-weight: 500; }
.divider { margin: 0 0.5rem; color: #999; }
.nav-user-link { display: flex; align-items: center; gap: 6px; text-decoration: none; color: #333; }
.nav-avatar { border-radius: 50%; object-fit: cover; }
.nav-user-name { color: #007bff; font-weight: 600; }
.nav-logout { color: #999 !important; font-size: 0.85rem; }
.nav-bell-item { position: relative; display: flex; align-items: center; }
.nav-bell-link { position: relative; padding: 0.5rem 0.6rem !important; font-size: 1.1rem !important; color: #555 !important; }
.nav-bell-link:hover { color: #007bff !important; }
.nav-bell-badge {
  position: absolute; top: 2px; right: 2px; min-width: 16px; height: 16px;
  background: #fb7299; color: #fff; font-size: 0.65rem; font-weight: 600;
  border-radius: 10px; display: flex; align-items: center; justify-content: center;
  padding: 0 4px; line-height: 1;
}
.mobile-bell-badge {
  display: inline-block; min-width: 18px; height: 18px;
  background: #fb7299; color: #fff; font-size: 0.7rem; font-weight: 600;
  border-radius: 10px; text-align: center; line-height: 18px;
  padding: 0 5px; margin-left: 6px; vertical-align: middle;
}
.mobile-menu-toggle { display: none; background: none; border: none; cursor: pointer; padding: 0.5rem; }
.hamburger-icon { display: block; width: 24px; height: 2px; background-color: #333; position: relative; transition: background-color 0.3s ease; }
.hamburger-icon:before, .hamburger-icon:after { content: ''; position: absolute; width: 24px; height: 2px; background-color: #333; transition: transform 0.3s ease; }
.hamburger-icon:before { top: -6px; }
.hamburger-icon:after { bottom: -6px; }
.mobile-menu-toggle[aria-expanded="true"] .hamburger-icon { background-color: transparent; }
.mobile-menu-toggle[aria-expanded="true"] .hamburger-icon:before { transform: rotate(45deg); top: 0; }
.mobile-menu-toggle[aria-expanded="true"] .hamburger-icon:after { transform: rotate(-45deg); bottom: 0; }
.mobile-menu { display: none; position: fixed; top: 60px; left: 0; width: 100%; background-color: #fff; box-shadow: 0 5px 10px rgba(0,0,0,0.1); padding: 1rem; z-index: 999; transform: translateY(-100%); opacity: 0; transition: transform 0.3s ease, opacity 0.3s ease; pointer-events: none; }
.mobile-menu.active { display: block; transform: translateY(0); opacity: 1; pointer-events: auto; }
.mobile-search { margin-bottom: 1rem; }
.mobile-nav-links, .mobile-user-links { list-style: none; padding: 0; margin: 0 0 1rem 0; }
.mobile-nav-item { margin: 0.5rem 0; }
.mobile-nav-item a { display: block; padding: 0.5rem 0; color: #333; text-decoration: none; font-weight: 500; }
.mobile-sub-links { list-style: none; padding: 0 0 0 1rem; margin: 0.3rem 0; }
.mobile-sub-item { margin: 0.2rem 0; }
.mobile-sub-item a { display: block; padding: 0.4rem 0.8rem; color: #666; text-decoration: none; font-size: 0.85rem; font-weight: 400; border-radius: 6px; }
.mobile-sub-item a:hover { background: #f5f7fa; color: #007bff; }
.mobile-auth { border-top: 1px solid #eee; padding-top: 1rem; }
.mobile-notice-link { display: block; margin-bottom: 1rem; color: #666; text-decoration: none; }
.mobile-login-register { display: flex; align-items: center; }
.mobile-login-button, .mobile-register-button { color: #333; text-decoration: none; font-weight: 500; }
.mobile-divider { margin: 0 0.5rem; color: #999; }
@media (max-width: 992px) {
  .desktop-only { display: none; }
  .mobile-menu-toggle { display: block; }
  .search-container { flex: 0 1 200px; }
  .mobile-menu { display: block; }
}
@media (max-width: 768px) {
  .search-container { display: none; }
  .nav-right .auth-links { display: none; }
}
@media (min-width: 993px) { .mobile-menu { display: none !important; } }
</style>
