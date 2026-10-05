export const useUser = () => {
  const user = useState('user', () => null)

  async function fetchUser() {
    try {
      const res = await $fetch('/api/user/me')
      user.value = res.user || null
    } catch {
      user.value = null
    }
  }

  function setUser(u: any) {
    user.value = u
  }

  return { user, fetchUser, setUser }
}
