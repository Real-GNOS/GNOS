export function usePWA() {
  const deferredPrompt = ref<any>(null)
  const canInstall = ref(false)

  function beforeInstallHandler(e: any) {
    e.preventDefault()
    deferredPrompt.value = e
    canInstall.value = true
  }

  async function install() {
    if (!deferredPrompt.value) return
    deferredPrompt.value.prompt()
    const result = await deferredPrompt.value.userChoice
    deferredPrompt.value = null
    canInstall.value = false
    return result.outcome === 'accepted'
  }

  async function registerSW() {
    if ('serviceWorker' in navigator) {
      try {
        const registration = await navigator.serviceWorker.register('/sw.js')
        console.log('SW registered:', registration.scope)
        return registration
      } catch (e) {
        console.error('SW registration failed:', e)
      }
    }
  }

  async function requestNotificationPermission() {
    if (!('Notification' in window)) return false
    const result = await Notification.requestPermission()
    return result === 'granted'
  }

  async function subscribePush(registration: ServiceWorkerRegistration) {
    try {
      const sub = await registration.pushManager.subscribe({
        userVisibleOnly: true,
        applicationServerKey: urlBase64ToUint8Array(process.env.VAPID_PUBLIC_KEY || ''),
      })
      await $fetch('/api/push-subscribe', {
        method: 'POST',
        body: { endpoint: sub.endpoint, keys: sub.toJSON().keys },
      })
      return true
    } catch (e) {
      console.error('Push subscription failed:', e)
      return false
    }
  }

  onMounted(() => {
    window.addEventListener('beforeinstallprompt', beforeInstallHandler)
    registerSW()

    if (window.matchMedia('(display-mode: standalone)').matches) {
      canInstall.value = false
    }
  })

  onUnmounted(() => {
    window.removeEventListener('beforeinstallprompt', beforeInstallHandler)
  })

  return { canInstall, install, registerSW, requestNotificationPermission, subscribePush }
}

function urlBase64ToUint8Array(base64String: string) {
  const padding = '='.repeat((4 - (base64String.length % 4)) % 4)
  const base64 = (base64String + padding).replace(/-/g, '+').replace(/_/g, '/')
  const rawData = window.atob(base64)
  return Uint8Array.from([...rawData].map((char) => char.charCodeAt(0)))
}
