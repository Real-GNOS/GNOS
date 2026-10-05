// 全局封禁状态：被封禁的当前登录用户本人可见，用于全局横幅与各写操作入口的禁用。
// 注意：/api/user/my-ban 基于 cookie 判断，被封禁用户调用不会抛 403，从而本人能看到自己被封禁。
export const useBan = () => {
  const banned = useState<boolean>('ban_banned', () => false)
  const banInfo = useState<any>('ban_info', () => null)

  async function fetchBan() {
    try {
      const res: any = await $fetch('/api/user/my-ban')
      banned.value = !!res.banned
      banInfo.value = res.ban || null
    } catch {
      banned.value = false
      banInfo.value = null
    }
  }

  function formatExpire(expiresAt: string) {
    if (!expiresAt) return ''
    try {
      return new Date(expiresAt).toLocaleString('zh-CN')
    } catch {
      return expiresAt
    }
  }

  return { banned, banInfo, fetchBan, formatExpire }
}
