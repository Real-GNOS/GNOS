<template>
  <div class="ad-section" v-if="show">
    <a :href="link">
      <img height="100%" width="100%" :src="imgSrc">
    </a>
  </div>
</template>

<script setup>
const props = defineProps({
  img: { type: String, default: '' },
  link: { type: String, default: '/' },
  show: { type: Boolean, default: true }
})
const imgSrc = computed(() => props.img || '/images/banner1.jpg')
</script>
