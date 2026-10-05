export default defineNuxtRouteMiddleware(async (to) => {
  if (!to.path.startsWith('/admin') || to.path === '/admin/login') return

  const event = useRequestEvent()
  const isServer = !!event
  const token = useCookie('cocokalo_token').value
  const userCookie = useCookie('cocokalo_user').value

  // 服务端能读到 httpOnly cookie；客户端 httpOnly 对 JS 不可见，
  // 但浏览器会自动把 cookie 随同源 $fetch 发出去，因此客户端导航时
  // 跳过本地 cookie 检查，交给 /api/user/me 校验。
  if (isServer && !token && !userCookie) {
    return navigateTo('/admin/login')
  }

  try {
    // SSR 下需手动把请求里的 cookie 转发给内部 $fetch，否则服务端认不出登录态
    const headers = isServer ? { cookie: getRequestHeader(event, 'cookie') || '' } : undefined
    const res: any = await $fetch('/api/user/me', { headers })
    if (!res?.user || res.user.role !== 'admin') {
      // 仅在服务端清除 cookie（客户端无法清除 httpOnly cookie，且不应误清有效会话）
      if (isServer) {
        useCookie('cocokalo_user').value = null
        useCookie('cocokalo_token').value = null
      }
      return navigateTo('/admin/login')
    }
  } catch {
    if (isServer) {
      useCookie('cocokalo_user').value = null
      useCookie('cocokalo_token').value = null
    }
    return navigateTo('/admin/login')
  }
})
