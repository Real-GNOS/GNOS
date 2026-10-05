<template>
  <div class="carousel-container" ref="carouselRef">
    <div class="carousel-track">
      <div v-for="(item, index) in items" :key="index"
        class="carousel-slide" :class="{ active: currentIndex === index }"
        role="group" aria-roledescription="slide"
        :aria-label="`${index + 1} of ${items.length}`">
        <a v-if="item.url" :href="item.url" class="carousel-link" :tabindex="currentIndex === index ? '0' : '-1'">
          <img :src="item.link" :alt="item.title || '轮播图片 ' + (index + 1)" class="carousel-image" :loading="index === 0 ? 'eager' : 'lazy'">
          <div v-if="item.title || item.description" class="carousel-caption">
            <h3 v-if="item.title" class="carousel-title">{{ item.title }}</h3>
            <p v-if="item.description" class="carousel-description">{{ item.description }}</p>
          </div>
        </a>
        <template v-else>
          <img :src="item.link" :alt="item.title || '轮播图片 ' + (index + 1)" class="carousel-image" :loading="index === 0 ? 'eager' : 'lazy'">
          <div v-if="item.title || item.description" class="carousel-caption">
            <h3 v-if="item.title" class="carousel-title">{{ item.title }}</h3>
            <p v-if="item.description" class="carousel-description">{{ item.description }}</p>
          </div>
        </template>
      </div>
    </div>

    <button class="carousel-control prev" aria-label="上一张" @click="goToPrev">
      <span class="carousel-control-icon" aria-hidden="true">&#10094;</span>
    </button>
    <button class="carousel-control next" aria-label="下一张" @click="goToNext">
      <span class="carousel-control-icon" aria-hidden="true">&#10095;</span>
    </button>

    <div class="carousel-indicators" role="tablist">
      <button v-for="(_, i) in items" :key="i"
        class="carousel-indicator" :class="{ active: currentIndex === i }"
        role="tab" :aria-label="'显示第 ' + (i + 1) + ' 张幻灯片'"
        :aria-selected="currentIndex === i ? 'true' : 'false'"
        :tabindex="currentIndex === i ? '0' : '-1'"
        @click="goTo(i)">
      </button>
    </div>

    <button class="carousel-autoplay-control" :aria-label="isAutoPlaying ? '暂停自动播放' : '开始自动播放'" @click="toggleAutoplay">
      <span class="pause-icon" :style="{ display: isAutoPlaying ? '' : 'none' }" aria-hidden="true">&#10074;&#10074;</span>
      <span class="play-icon" :style="{ display: isAutoPlaying ? 'none' : '' }" aria-hidden="true">&#9658;</span>
    </button>
  </div>
</template>

<script setup>
const props = defineProps({ items: { type: Array, default: () => [] } })

const carouselRef = ref(null)
const currentIndex = ref(0)
const isAutoPlaying = ref(true)
let autoplayInterval = null
const autoplayDelay = 5000

function goTo(index) {
  currentIndex.value = index
  resetAutoplay()
}

function goToPrev() {
  currentIndex.value = (currentIndex.value - 1 + props.items.length) % props.items.length
  resetAutoplay()
}

function goToNext() {
  currentIndex.value = (currentIndex.value + 1) % props.items.length
  resetAutoplay()
}

function startAutoplay() {
  if (autoplayInterval) clearInterval(autoplayInterval)
  autoplayInterval = setInterval(goToNext, autoplayDelay)
}

function pauseAutoplay() {
  if (autoplayInterval) {
    clearInterval(autoplayInterval)
    autoplayInterval = null
  }
}

function resetAutoplay() {
  if (isAutoPlaying.value) {
    pauseAutoplay()
    startAutoplay()
  }
}

function toggleAutoplay() {
  isAutoPlaying.value = !isAutoPlaying.value
  if (isAutoPlaying.value) startAutoplay()
  else pauseAutoplay()
}

function handleKeyDown(e) {
  switch (e.key) {
    case 'ArrowLeft': goToPrev(); break
    case 'ArrowRight': goToNext(); break
    case 'Home': goTo(0); break
    case 'End': goTo(props.items.length - 1); break
    case ' ': toggleAutoplay(); break
  }
}

let touchStartX = 0
function handleTouchStart(e) { touchStartX = e.changedTouches[0].screenX }
function handleTouchEnd(e) {
  const diff = touchStartX - e.changedTouches[0].screenX
  if (diff > 50) goToNext()
  else if (diff < -50) goToPrev()
}

onMounted(() => {
  if (props.items.length > 1) {
    startAutoplay()
    const el = carouselRef.value
    if (el) {
      el.addEventListener('keydown', handleKeyDown)
      el.addEventListener('mouseenter', pauseAutoplay)
      el.addEventListener('mouseleave', () => { if (isAutoPlaying.value) startAutoplay() })
      el.addEventListener('touchstart', handleTouchStart, { passive: true })
      el.addEventListener('touchend', handleTouchEnd, { passive: true })
    }
  }
})

onUnmounted(() => { pauseAutoplay() })
</script>

<style scoped>
.carousel-container { position: relative; width: 100%; height: 0; padding-bottom: 56.25%; overflow: hidden; border-radius: 8px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); }
.carousel-track { position: absolute; top: 0; left: 0; width: 100%; height: 100%; }
.carousel-slide { position: absolute; top: 0; left: 0; width: 100%; height: 100%; opacity: 0; transition: opacity 0.5s ease; z-index: 1; }
.carousel-slide.active { opacity: 1; z-index: 2; }
.carousel-image { width: 100%; height: 100%; object-fit: cover; display: block; }
.carousel-link { display: block; width: 100%; height: 100%; color: inherit; text-decoration: none; }
.carousel-caption { position: absolute; bottom: 0; left: 0; right: 0; padding: 20px; background: linear-gradient(to top, rgba(0,0,0,0.7), rgba(0,0,0,0)); color: #fff; text-align: left; }
.carousel-title { margin: 0 0 8px; font-size: 1.5rem; font-weight: 600; }
.carousel-description { margin: 0; font-size: 1rem; opacity: 0.9; }
.carousel-control { position: absolute; top: 50%; transform: translateY(-50%); width: 40px; height: 40px; background-color: rgba(255,255,255,0.5); border: none; border-radius: 50%; cursor: pointer; display: flex; align-items: center; justify-content: center; z-index: 10; opacity: 0; transition: all 0.3s ease; }
.carousel-container:hover .carousel-control { opacity: 1; }
.carousel-control.prev { left: 10px; }
.carousel-control.next { right: 10px; }
.carousel-control:hover { background-color: rgba(255,255,255,0.8); }
.carousel-indicators { position: absolute; bottom: 10px; left: 50%; transform: translateX(-50%); display: flex; gap: 8px; z-index: 10; }
.carousel-indicator { width: 10px; height: 10px; border-radius: 50%; background-color: rgba(255,255,255,0.5); border: none; cursor: pointer; padding: 0; transition: all 0.3s ease; }
.carousel-indicator.active { background-color: #fff; transform: scale(1.2); }
.carousel-autoplay-control { position: absolute; bottom: 10px; right: 10px; width: 30px; height: 30px; background-color: rgba(0,0,0,0.5); border: none; border-radius: 50%; cursor: pointer; display: flex; align-items: center; justify-content: center; z-index: 10; color: #fff; font-size: 12px; opacity: 0; transition: opacity 0.3s ease; }
.carousel-container:hover .carousel-autoplay-control { opacity: 1; }
</style>
