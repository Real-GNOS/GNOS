<template>
  <div class="danmaku-stage" ref="stageRef">
    <canvas ref="canvasRef" class="danmaku-canvas" width="640" height="360" />
    <div class="dm-extra">
      <span class="dm-count" v-if="totalCount"><i class="fa fa-commenting"></i> {{ totalCount }}</span>
      <div class="dm-controls">
        <button class="dm-btn" @click="toggleInput" title="发送弹幕">
          <i class="fa fa-commenting"></i>
        </button>
        <button class="dm-btn" @click="toggleSettings" title="弹幕设置">
          <i class="fa fa-cog"></i>
        </button>
      </div>
    </div>
    <div class="dm-settings" v-if="showSettings" @click.stop>
      <label>不透明度 <input type="range" min="10" max="100" v-model.number="opacity"></label>
      <label>字号
        <select v-model.number="fontSize">
          <option :value="12">小</option>
          <option :value="14">中</option>
          <option :value="18">大</option>
          <option :value="24">特大</option>
        </select>
      </label>
      <label>显示时长
        <select v-model.number="displayDuration">
          <option :value="3">3秒</option>
          <option :value="5">5秒</option>
          <option :value="8">8秒</option>
          <option :value="12">12秒</option>
        </select>
      </label>
      <label>弹幕密度
        <select v-model.number="density">
          <option :value="0.33">低</option>
          <option :value="0.5">中</option>
          <option :value="0.75">高</option>
          <option :value="1">满</option>
        </select>
      </label>
      <label>显示区域
        <select v-model="area">
          <option value="all">全部</option>
          <option value="top">上半屏</option>
          <option value="half">半屏</option>
        </select>
      </label>
    </div>
    <div class="dm-input" v-if="showInput" @click.stop>
      <div class="dm-input-row">
        <select v-model="dmType" class="dm-type-select">
          <option value="scroll">滚动</option>
          <option value="top">顶部</option>
          <option value="bottom">底部</option>
        </select>
        <div class="dm-color-select">
          <button v-for="c in dmColors" :key="c"
            :class="{ active: dmColor === c }"
            :style="{ background: c }"
            @click="dmColor = c">
          </button>
        </div>
      </div>
      <div class="dm-input-row">
        <input v-model="dmText" type="text" maxlength="100" placeholder="发个弹幕~"
          @keyup.enter="send" ref="inputRef">
        <button @click="send" :disabled="!dmText.trim()" class="dm-send-btn">发送</button>
      </div>
    </div>
    <div v-if="showInput || showSettings" class="dm-overlay-close" @click="showInput = false; showSettings = false" />
  </div>
</template>

<script setup>
const props = defineProps({
  slug: { type: String, required: true },
  player: { type: Object, default: null },
})

const SEGMENT_DURATION = 360
const SCROLL_TRACKS = 14
const TRACK_HEIGHT = 26
const BASE_SCROLL_SPEED = 400

const stageRef = ref(null)
const canvasRef = ref(null)
const inputRef = ref(null)
const dmText = ref('')
const dmType = ref('scroll')
const dmColor = ref('#ffffff')
const showInput = ref(false)
const showSettings = ref(false)
const totalCount = ref(0)
const dmColors = ['#ffffff', '#ff0000', '#00ff00', '#ffff00', '#00a1d6', '#ff69b4', '#ffa500', '#9b59b6']

const opacity = ref(80)
const fontSize = ref(14)
const displayDuration = ref(5)
const density = ref(0.5)
const area = ref('all')

let allDanmaku = []
let activeDanmaku = new Map()
let segmentsLoaded = new Set()
let currentSegment = -1
let canvas = null
let ctx = null
let animId = null
let es = null
let esReconnectTimer = null
let videoTime = 0
let playing = false
let nextId = 1

onMounted(() => {
  opacity.value = parseInt(localStorage.getItem('dm_opacity') || '80')
  fontSize.value = parseInt(localStorage.getItem('dm_fontSize') || '14')
  displayDuration.value = parseInt(localStorage.getItem('dm_duration') || '5')
  density.value = parseFloat(localStorage.getItem('dm_density') || '0.5')
  area.value = localStorage.getItem('dm_area') || 'all'
  initCanvas()
  connectSSE()
})

onUnmounted(() => {
  if (animId) cancelAnimationFrame(animId)
  if (esReconnectTimer) clearTimeout(esReconnectTimer)
  if (es) { es.onerror = null; es.close() }
})

watch(opacity, v => { if (process.client) localStorage.setItem('dm_opacity', String(v)) })
watch(fontSize, v => { if (process.client) localStorage.setItem('dm_fontSize', String(v)) })
watch(displayDuration, v => { if (process.client) localStorage.setItem('dm_duration', String(v)) })
watch(density, v => { if (process.client) localStorage.setItem('dm_density', String(v)) })
watch(area, v => { if (process.client) localStorage.setItem('dm_area', v) })

watch(() => props.player, (p) => {
  if (!p) return
  p.on('timeupdate', () => { videoTime = p.currentTime })
  p.on('play', () => { playing = true })
  p.on('pause', () => { playing = false })
  p.on('seeking', () => {
    videoTime = p.currentTime
    segmentsLoaded.clear()
    currentSegment = -1
    activeDanmaku.clear()
  })
})

function initCanvas() {
  if (!canvasRef.value) return
  canvas = canvasRef.value
  ctx = canvas.getContext('2d')
  resizeCanvas()
  window.addEventListener('resize', resizeCanvas)
  startRender()
}

function resizeCanvas() {
  if (!canvas || !stageRef.value) return
  const rect = stageRef.value.getBoundingClientRect()
  canvas.width = rect.width
  canvas.height = rect.height
}

function connectSSE() {
  if (es) { es.close() }
  try {
    es = new EventSource(`/api/danmaku/${props.slug}/sse`)
    es.onmessage = (e) => {
      try {
        const data = JSON.parse(e.data)
        if (data.type === 'danmaku' && data.danmaku) {
          const d = data.danmaku
          allDanmaku.push({
            id: d.id || nextId++,
            content: d.content,
            time: d.time,
            type: d.type || 'scroll',
            color: d.color || '#ffffff',
            username: d.username || '匿名',
          })
          totalCount.value = allDanmaku.length
        }
      } catch {}
    }
    es.onerror = () => {
      es.close()
      esReconnectTimer = setTimeout(() => connectSSE(), 3000)
    }
  } catch {
    esReconnectTimer = setTimeout(() => connectSSE(), 3000)
  }
}

function loadSegment(seg) {
  if (segmentsLoaded.has(seg) || seg < 0) return
  segmentsLoaded.add(seg)
  $fetch(`/api/danmaku/${props.slug}?segment=${seg}`).then(res => {
    if (res?.data) {
      for (const d of res.data) {
        if (!allDanmaku.find(ex => ex.id === d.id)) {
          allDanmaku.push({
            id: d.id,
            content: d.content,
            time: d.time,
            type: d.type || 'scroll',
            color: d.color || '#ffffff',
            username: d.username || '匿名',
          })
        }
      }
      allDanmaku.sort((a, b) => a.time - b.time)
      totalCount.value = allDanmaku.length
    }
  }).catch(() => {})
}

function getScrollSpeed(text) {
  return BASE_SCROLL_SPEED * (1 + Math.min(text.length / 30, 2))
}

function startRender() {
  let lastCleanup = 0

  function render(now) {
    animId = requestAnimationFrame(render)
    if (!ctx || !canvas) return

    const ct = videoTime
    const seg = Math.floor(ct / SEGMENT_DURATION)
    if (seg !== currentSegment) {
      currentSegment = seg
      loadSegment(seg)
      loadSegment(seg + 1)
      if (seg > 1) segmentsLoaded.delete(seg - 2)
    }

    ctx.clearRect(0, 0, canvas.width, canvas.height)

    const a = opacity.value / 100
    const size = fontSize.value
    const w = canvas.width
    const h = canvas.height
    const dur = displayDuration.value
    const dens = density.value
    const maxTracks = Math.round(SCROLL_TRACKS * dens)

    if (a < 0.01) return

    const nowSec = now / 1000
    const trackBusyUntil = new Float64Array(SCROLL_TRACKS)

    if (nowSec - lastCleanup > 10) {
      lastCleanup = nowSec
      const cutoff = ct - dur - 2
      allDanmaku = allDanmaku.filter(d => d.time > cutoff)
    }

    ctx.font = `bold ${size}px sans-serif`
    ctx.textBaseline = 'top'
    ctx.shadowColor = 'rgba(0,0,0,0.8)'
    ctx.shadowBlur = 4
    ctx.shadowOffsetX = 1
    ctx.shadowOffsetY = 1

    const trackOffset = area.value === 'top' ? 0
      : area.value === 'half' ? Math.floor(SCROLL_TRACKS / 4)
      : 0
    const trackLimit = area.value === 'half'
      ? trackOffset + Math.floor(SCROLL_TRACKS / 2)
      : maxTracks

    let drawn = 0
    const maxDraw = Math.round(40 * dens)

    for (const d of allDanmaku) {
      if (d.type !== 'scroll') continue
      if (d.time > ct + dur || d.time < ct - 2) continue
      if (drawn >= maxDraw) break

      const speed = getScrollSpeed(d.content)
      const textW = ctx.measureText(d.content).width + 40
      const scrollTime = (w + textW) / speed
      const age = nowSec - (d._startTime || nowSec)
      if (d._startTime === undefined) d._startTime = nowSec

      if (age > dur) continue
      const progress = age / scrollTime
      if (progress > 1) continue

      const x = w - progress * (w + textW)
      if (x < -textW) continue

      let lane = -1
      let best = Infinity
      for (let i = trackOffset; i < trackLimit; i++) {
        if (trackBusyUntil[i] <= nowSec) { lane = i; break }
        if (trackBusyUntil[i] < best) { best = trackBusyUntil[i]; lane = i }
      }

      const yVal = 8 + lane * TRACK_HEIGHT
      trackBusyUntil[lane] = nowSec + (scrollTime - progress * scrollTime) + 0.1

      const fadeIn = Math.min(age / 0.5, 1)
      const fadeOut = age > dur - 1 ? Math.max(0, (dur - age)) : 1
      ctx.globalAlpha = a * fadeIn * fadeOut
      ctx.fillStyle = d.color
      ctx.fillText(d.content, x, yVal)
      drawn++
    }

    for (const d of allDanmaku) {
      if (d.type !== 'top' && d.type !== 'bottom') continue
      if (d.time > ct + 1 || d.time < ct - dur) continue
      if (drawn >= maxDraw) break

      const age = nowSec - (d._startTime || nowSec)
      if (d._startTime === undefined) d._startTime = nowSec
      if (age > dur) continue

      const textW = ctx.measureText(d.content).width
      const xPos = (w - textW) / 2
      const yPos = d.type === 'top' ? 8 : h - 32 - size

      const fadeIn = Math.min(age / 0.5, 1)
      const fadeOut = age > dur - 1 ? Math.max(0, dur - age) : 1
      ctx.globalAlpha = a * fadeIn * fadeOut
      ctx.fillStyle = d.color
      ctx.fillText(d.content, xPos, yPos)
      drawn++
    }

    ctx.shadowBlur = 0
    ctx.globalAlpha = 1
  }
  animId = requestAnimationFrame(render)
}

function toggleInput() {
  showInput.value = !showInput.value
  showSettings.value = false
  if (showInput.value) nextTick(() => inputRef.value?.focus())
}

function toggleSettings() {
  showSettings.value = !showSettings.value
  showInput.value = false
}

async function send() {
  const t = dmText.value.trim()
  if (!t || !props.player) return
  const time = props.player.currentTime
  try {
    await $fetch(`/api/danmaku/${props.slug}`, {
      method: 'POST',
      body: { content: t, time, type: dmType.value, color: dmColor.value }
    })
    dmText.value = ''
  } catch {}
}
</script>

<style scoped>
.danmaku-stage {
  position: absolute; inset: 0; z-index: 5;
  pointer-events: none; overflow: hidden;
}
.danmaku-canvas {
  position: absolute; inset: 0;
  width: 100%; height: 100%;
  pointer-events: none;
}
.dm-extra {
  position: absolute; top: 8px; left: 10px;
  display: flex; align-items: center; gap: 8px;
  pointer-events: none; z-index: 10;
}
.dm-count {
  color: rgba(255,255,255,0.7); font-size: 12px;
  background: rgba(0,0,0,0.4); padding: 2px 8px; border-radius: 10px;
}
.dm-controls { display: flex; gap: 6px; pointer-events: auto; }
.dm-btn {
  background: rgba(0,0,0,0.5); border: none; color: #fff;
  width: 34px; height: 34px; border-radius: 50%; cursor: pointer;
  font-size: 15px; display: flex; align-items: center;
  justify-content: center; transition: background 0.2s;
}
.dm-btn:hover { background: rgba(0,0,0,0.75); }
.dm-settings {
  position: absolute; top: 8px; right: 10px; z-index: 20;
  background: rgba(0,0,0,0.85); border-radius: 8px; padding: 12px 14px;
  display: flex; flex-direction: column; gap: 8px; min-width: 180px;
  pointer-events: auto;
}
.dm-settings label {
  display: flex; align-items: center; justify-content: space-between;
  gap: 8px; font-size: 12px; color: #ccc;
}
.dm-settings input[type=range] { width: 80px; }
.dm-settings select {
  background: #333; color: #fff; border: 1px solid #555;
  border-radius: 4px; padding: 2px 6px; font-size: 12px;
}
.dm-input {
  position: absolute; bottom: 60px; left: 50%; transform: translateX(-50%);
  z-index: 20; pointer-events: auto;
  background: rgba(0,0,0,0.8); border-radius: 10px; padding: 8px 12px;
  display: flex; flex-direction: column; gap: 6px; min-width: 300px;
}
.dm-input-row { display: flex; gap: 6px; align-items: center; }
.dm-type-select {
  background: rgba(255,255,255,0.1); border: 1px solid rgba(255,255,255,0.2);
  color: #ddd; border-radius: 4px; padding: 4px 8px; font-size: 11px;
}
.dm-color-select { display: flex; gap: 4px; }
.dm-color-select button {
  width: 18px; height: 18px; border-radius: 50%;
  border: 2px solid transparent; cursor: pointer;
}
.dm-color-select button.active { border-color: #fff; }
.dm-input input {
  flex: 1; background: rgba(255,255,255,0.1);
  border: 1px solid rgba(255,255,255,0.2); color: #fff;
  outline: none; font-size: 13px; padding: 6px 10px; border-radius: 6px;
}
.dm-input input::placeholder { color: rgba(255,255,255,0.4); }
.dm-send-btn {
  background: #00a1d6; border: none; color: #fff; cursor: pointer;
  font-size: 12px; padding: 6px 14px; border-radius: 6px; font-weight: 600;
}
.dm-send-btn:disabled { opacity: 0.4; cursor: default; }
.dm-overlay-close {
  position: fixed; inset: 0; z-index: 15;
}
</style>
