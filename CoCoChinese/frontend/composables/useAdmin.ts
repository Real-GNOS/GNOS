import { ElMessageBox } from 'element-plus'

export function useAdmin() {
  const router = useRouter()
  const route = useRoute()
  const { user } = useUser()
  const { success, error: showError, info } = useToast()

  const isAdmin = computed(() => user.value?.role === 'admin')

  const sidebarCollapsed = ref(false)

  function toggleSidebar() {
    sidebarCollapsed.value = !sidebarCollapsed.value
  }

  function logout() {
    document.cookie = 'cocokalo_user=; Max-Age=0; path=/'
    document.cookie = 'cocokalo_token=; Max-Age=0; path=/'
    navigateTo('/admin/login')
  }

  function confirmAction(message: string, title = '确认操作'): Promise<boolean> {
    return ElMessageBox.confirm(message, title, {
      confirmButtonText: '确定',
      cancelButtonText: '取消',
      type: 'warning',
      roundButton: true,
      draggable: true,
    }).then(() => true).catch(() => false)
  }

  async function confirmDelete(message = '真的要消除这条记录吗？ ✿') {
    return confirmAction(`<i class="fa fa-exclamation-triangle" style="color:#ff5252"></i> ${message}`, '⚠️ 危险操作')
  }

  function formatDate(date: string | Date, fmt = 'YYYY-MM-DD HH:mm:ss') {
    const d = new Date(date)
    const pad = (n: number) => String(n).padStart(2, '0')
    return fmt
      .replace('YYYY', String(d.getFullYear()))
      .replace('MM', pad(d.getMonth() + 1))
      .replace('DD', pad(d.getDate()))
      .replace('HH', pad(d.getHours()))
      .replace('mm', pad(d.getMinutes()))
      .replace('ss', pad(d.getSeconds()))
  }

  function timeAgo(date: string | Date) {
    const diff = Date.now() - new Date(date).getTime()
    const mins = Math.floor(diff / 60000)
    if (mins < 1) return '刚刚'
    if (mins < 60) return `${mins} 分钟前`
    const hours = Math.floor(mins / 60)
    if (hours < 24) return `${hours} 小时前`
    const days = Math.floor(hours / 24)
    if (days < 30) return `${days} 天前`
    return formatDate(date, 'YYYY-MM-DD')
  }

  function formatBytes(bytes: number) {
    if (!bytes) return '0 B'
    const units = ['B', 'KB', 'MB', 'GB', 'TB']
    let i = 0
    for (; bytes >= 1024 && i < units.length - 1; i++) bytes /= 1024
    return `${bytes.toFixed(1)} ${units[i]}`
  }

  function formatNumber(num: number | string) {
    const n = Number(num)
    if (isNaN(n)) return '0'
    if (n >= 10000) return (n / 10000).toFixed(1) + '万'
    return String(n)
  }

  return {
    isAdmin,
    sidebarCollapsed,
    toggleSidebar,
    logout,
    confirmAction,
    confirmDelete,
    formatDate,
    timeAgo,
    formatBytes,
    formatNumber,
    success,
    error: showError,
    info,
  }
}
