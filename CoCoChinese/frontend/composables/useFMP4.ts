interface VideoJson {
  duration: number
  segmentDuration: number
  resolutions: Record<string, {
    width: number
    height: number
    videoInit: string
    audioInit: string
    videoSegments: { url: string; start: number; end: number }[]
    audioSegments: { url: string; start: number; end: number }[]
    videoCodec: string
    audioCodec: string
  }>
}

export function useFMP4() {
  let mediaSource: MediaSource | null = null
  let videoSB: SourceBuffer | null = null
  let audioSB: SourceBuffer | null = null
  let videoEl: HTMLVideoElement | null = null
  let manifest: VideoJson | null = null
  let currentResolution = ''
  let token = ''
  let slug = ''
  let heartbeatTimer: ReturnType<typeof setInterval> | null = null
  let loadedInits = new Set<string>()
  let loadedVideoSegs = new Set<number>()
  let loadedAudioSegs = new Set<number>()
  let appending = false
  let destroyRequested = false
  let seekPending = false

  function setToken(newToken: string) { token = newToken }

  function appendBuffer(sb: SourceBuffer, data: ArrayBuffer): Promise<void> {
    return new Promise((resolve, reject) => {
      if (destroyRequested) { resolve(); return }
      const onEnd = () => { sb.removeEventListener('updateend', onEnd); sb.removeEventListener('error', onErr); resolve() }
      const onErr = (e: Event) => { sb.removeEventListener('updateend', onEnd); sb.removeEventListener('error', onErr); reject(e) }
      sb.addEventListener('updateend', onEnd, { once: true })
      sb.addEventListener('error', onErr, { once: true })
      sb.appendBuffer(data)
    })
  }

  async function fetchSegment(url: string): Promise<ArrayBuffer | null> {
    try {
      const fullUrl = `/api/fmp4/${slug}/${url}`
      const sep = fullUrl.includes('?') ? '&' : '?'
      const res = await fetch(fullUrl + sep + '_vt=' + encodeURIComponent(token))
      if (!res.ok) return null
      return await res.arrayBuffer()
    } catch {
      return null
    }
  }

  async function loadInitSegments(res: string) {
    if (!manifest || !videoSB || !audioSB) return
    const key = `init_${res}`
    if (loadedInits.has(key)) return
    loadedInits.add(key)

    const info = manifest.resolutions[res]
    if (!info) return

    const [vData, aData] = await Promise.all([
      fetchSegment(info.videoInit),
      fetchSegment(info.audioInit),
    ])

    if (destroyRequested) return
    if (vData && !videoSB.updating) await appendBuffer(videoSB, vData).catch(() => {})
    if (destroyRequested) return
    if (aData && !audioSB.updating) await appendBuffer(audioSB, aData).catch(() => {})
  }

  function getSegmentIndex(segments: { start: number; end: number }[], time: number): number {
    for (let i = 0; i < segments.length; i++) {
      if (time >= segments[i].start - 0.1 && time < segments[i].end + 0.1) return i
    }
    if (segments.length > 0 && time < segments[0].start) return 0
    return segments.length - 1
  }

  async function loadMediaSegments() {
    if (!manifest || !videoEl || !videoSB || !audioSB || appending) return
    const res = currentResolution
    const info = manifest.resolutions[res]
    if (!info) return

    const ct = videoEl.currentTime
    const preloadCount = 3

    const vIdx = getSegmentIndex(info.videoSegments, ct)
    const aIdx = getSegmentIndex(info.audioSegments, ct)

    appending = true
    try {
      for (let i = vIdx; i < Math.min(vIdx + preloadCount, info.videoSegments.length); i++) {
        if (destroyRequested) break
        const segKey = `v_${res}_${i}`
        if (loadedVideoSegs.has(segKey)) continue
        if (videoSB.updating) {
          await new Promise<void>(r => {
            const onUpd = () => { videoSB!.removeEventListener('updateend', onUpd); r() }
            videoSB!.addEventListener('updateend', onUpd, { once: true })
          })
        }
        if (destroyRequested) break
        loadedVideoSegs.add(segKey)
        const data = await fetchSegment(info.videoSegments[i].url)
        if (data && !destroyRequested && videoSB && !videoSB.updating) {
          await appendBuffer(videoSB, data).catch(() => {})
        }
      }

      for (let i = aIdx; i < Math.min(aIdx + preloadCount, info.audioSegments.length); i++) {
        if (destroyRequested) break
        const segKey = `a_${res}_${i}`
        if (loadedAudioSegs.has(segKey)) continue
        if (audioSB.updating) {
          await new Promise<void>(r => {
            const onUpd = () => { audioSB!.removeEventListener('updateend', onUpd); r() }
            audioSB!.addEventListener('updateend', onUpd, { once: true })
          })
        }
        if (destroyRequested) break
        loadedAudioSegs.add(segKey)
        const data = await fetchSegment(info.audioSegments[i].url)
        if (data && !destroyRequested && audioSB && !audioSB.updating) {
          await appendBuffer(audioSB, data).catch(() => {})
        }
      }
    } finally {
      appending = false
    }
  }

  function startHeartbeat() {
    stopHeartbeat()
    heartbeatTimer = setInterval(() => {
      if (!destroyRequested && videoEl && !videoEl.paused) {
        loadMediaSegments()
      }
    }, 4000)
  }

  function stopHeartbeat() {
    if (heartbeatTimer) {
      clearInterval(heartbeatTimer)
      heartbeatTimer = null
    }
  }

  async function loadVideoJson(slugVal: string, tokenVal: string): Promise<VideoJson | null> {
    try {
      const res = await $fetch<VideoJson>(`/api/fmp4/${slugVal}/video.json`)
      return res
    } catch {
      return null
    }
  }

  async function init(
    slugVal: string,
    tokenVal: string,
    videoElement: HTMLVideoElement,
    resolution: string,
  ): Promise<boolean> {
    slug = slugVal
    token = tokenVal
    videoEl = videoElement
    destroyRequested = false

    manifest = await loadVideoJson(slug, token)
    if (!manifest || !manifest.resolutions[resolution]) return false

    currentResolution = resolution

    if (!window.MediaSource) return false

    mediaSource = new MediaSource()
    videoEl.src = URL.createObjectURL(mediaSource)

    await new Promise<void>((resolve) => {
      mediaSource!.addEventListener('sourceopen', () => resolve(), { once: true })
    })

    if (destroyRequested) return false

    const info = manifest.resolutions[currentResolution]
    const videoMime = `video/mp4; codecs="${info.videoCodec}"`
    const audioMime = `audio/mp4; codecs="${info.audioCodec}"`

    if (!MediaSource.isTypeSupported(videoMime)) {
      console.warn('fMP4: video MIME not supported:', videoMime)
      return false
    }

    try {
      videoSB = mediaSource.addSourceBuffer(videoMime)
      audioSB = mediaSource.addSourceBuffer(audioMime)
    } catch (e) {
      console.error('fMP4: addSourceBuffer failed:', e)
      return false
    }

    videoSB.mode = 'segments'
    audioSB.mode = 'segments'

    await loadInitSegments(currentResolution)
    if (destroyRequested) return false

    await loadMediaSegments()

    videoEl.addEventListener('seeking', onSeeking)
    videoEl.addEventListener('play', onPlay)

    startHeartbeat()
    return true
  }

  function onSeeking() {
    if (!videoEl || appending) return
    loadedVideoSegs.clear()
    loadedAudioSegs.clear()
    loadMediaSegments()
  }

  function onPlay() {
    loadMediaSegments()
  }

  async function switchResolution(newRes: string) {
    if (newRes === currentResolution || !manifest || !videoEl) return

    const ct = videoEl.currentTime
    const wasPlaying = !videoEl.paused

    destroy()
    await init(slug, token, videoEl, newRes)

    if (videoEl) {
      videoEl.currentTime = ct
      if (wasPlaying) videoEl.play().catch(() => {})
    }
  }

  function destroy() {
    destroyRequested = true
    stopHeartbeat()
    videoEl?.removeEventListener('seeking', onSeeking)
    videoEl?.removeEventListener('play', onPlay)

    if (videoSB && mediaSource && !videoSB.updating) {
      try { mediaSource.removeSourceBuffer(videoSB) } catch {}
    }
    if (audioSB && mediaSource && !audioSB.updating) {
      try { mediaSource.removeSourceBuffer(audioSB) } catch {}
    }
    if (mediaSource && mediaSource.readyState === 'open') {
      try { mediaSource.endOfStream() } catch {}
    }
    if (videoEl?.src) {
      URL.revokeObjectURL(videoEl.src)
      videoEl.removeAttribute('src')
    }

    mediaSource = null
    videoSB = null
    audioSB = null
    loadedInits.clear()
    loadedVideoSegs.clear()
    loadedAudioSegs.clear()
    appending = false
  }

  return { init, destroy, switchResolution, setToken }
}
