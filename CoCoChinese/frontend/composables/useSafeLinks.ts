export function useSafeLinks() {
  const URL_REGEX = /(https?:\/\/[^\s<>"')\]]+)/g

  function escapeHtml(text: string): string {
    return text
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;')
  }

  function renderLinks(text: string): string {
    if (!text) return ''
    const escaped = escapeHtml(text)
    return escaped.replace(URL_REGEX, (url) => {
      const cleanUrl = url.replace(/[,.;:!?]+$/, '')
      const rest = url.slice(cleanUrl.length)
      const encoded = encodeURIComponent(cleanUrl)
      return `<a href="/go/${encoded}" class="desc-link" target="_blank" rel="noopener noreferrer">${cleanUrl}</a>${rest}`
    }).replace(/\n/g, '<br>')
  }

  function insertLinkAtCursor(textarea: HTMLTextAreaElement, linkText: string): string {
    const start = textarea.selectionStart
    const end = textarea.selectionEnd
    const before = textarea.value.substring(0, start)
    const after = textarea.value.substring(end)
    return before + linkText + after
  }

  return { renderLinks, insertLinkAtCursor }
}
