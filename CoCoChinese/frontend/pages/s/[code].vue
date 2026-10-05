<template>
  <div class="share-redirect">
    <i class="fa fa-spinner fa-spin fa-3x"></i>
    <p>正在跳转...</p>
  </div>
</template>

<script setup lang="ts">
const route = useRoute()

onMounted(async () => {
  const code = route.params.code as string
  try {
    const res: any = await $fetch(`/api/share/${code}`)
    if (res.url) {
      window.location.href = res.url
    } else {
      navigateTo('/')
    }
  } catch {
    navigateTo('/')
  }
})
</script>

<style scoped>
.share-redirect { display: flex; flex-direction: column; align-items: center; justify-content: center; min-height: 60vh; color: var(--fluent-text-secondary); gap: var(--fluent-spacing-md); }
</style>
