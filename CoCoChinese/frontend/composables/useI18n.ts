export function useI18n() {
  const langState = useState('locale', () => 'zh-CN')
  const translationsState = useState('translations', () => ({} as Record<string, string>))
  const locale = computed(() => langState.value)
  const translations = computed(() => translationsState.value)

  function t(key: string, fallback?: string): string {
    return translations.value[key] || fallback || key
  }

  async function setLocale(lang: string) {
    try {
      const res = await $fetch(`/api/i18n/translations?lang=${lang}`)
      translationsState.value = res as Record<string, string>
      langState.value = lang
      document.documentElement.lang = lang
      try { localStorage.setItem('cocokalo_locale', lang) } catch {}
    } catch (e) {
      console.error('Failed to load translations:', e)
    }
  }

  async function initLocale() {
    let saved = 'zh-CN'
    try { saved = localStorage.getItem('cocokalo_locale') || 'zh-CN' } catch {}
    await setLocale(saved)
  }

  return { locale, translations, t, setLocale, initLocale }
}
