<template>
  <div class="fluent-card" :class="{ 'fluent-card-hover': hover, 'fluent-card-selected': selected }">
    <div v-if="header" class="fluent-card-header">
      <h3 v-if="header" class="fluent-card-title">{{ header }}</h3>
      <slot name="actions" />
    </div>
    <div class="fluent-card-body">
      <slot />
    </div>
    <div v-if="$slots.footer" class="fluent-card-footer">
      <slot name="footer" />
    </div>
  </div>
</template>

<script setup lang="ts">
defineProps({
  header: String,
  hover: { type: Boolean, default: true },
  selected: Boolean,
})
</script>

<style scoped>
.fluent-card {
  background: var(--fluent-bg-card);
  border: 1px solid var(--fluent-border);
  border-radius: var(--fluent-radius-lg);
  box-shadow: var(--fluent-shadow-card);
  transition: box-shadow var(--fluent-animation-duration) var(--fluent-animation-easing),
              transform var(--fluent-animation-duration) var(--fluent-animation-easing);
  animation: fluent-fade-in var(--fluent-animation-duration) var(--fluent-animation-easing);
}
.fluent-card-hover:hover {
  box-shadow: var(--fluent-shadow-card-hover);
  transform: translateY(-1px);
}
.fluent-card-selected {
  border-color: var(--fluent-accent);
  box-shadow: 0 0 0 1px var(--fluent-accent);
}
.fluent-card-header {
  display: flex; align-items: center; justify-content: space-between;
  padding: var(--fluent-spacing-lg) var(--fluent-spacing-xl) 0;
}
.fluent-card-title {
  font-size: var(--fluent-font-size-title);
  font-weight: 600; margin: 0; color: var(--fluent-text);
}
.fluent-card-body { padding: var(--fluent-spacing-xl); }
.fluent-card-footer {
  padding: var(--fluent-spacing-md) var(--fluent-spacing-xl);
  border-top: 1px solid var(--fluent-border);
  background: var(--fluent-bg-subtle);
  border-radius: 0 0 var(--fluent-radius-lg) var(--fluent-radius-lg);
}
</style>
