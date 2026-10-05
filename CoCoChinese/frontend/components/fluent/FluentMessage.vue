<template>
  <div :class="['fluent-message', `fluent-message-${type}`]">
    <i :class="iconClass" class="fluent-message-icon"></i>
    <div class="fluent-message-content">
      <slot />
    </div>
  </div>
</template>

<script setup lang="ts">
const props = defineProps({
  type: { type: String, default: 'info', validator: v => ['info', 'success', 'warning', 'error'].includes(v) },
})

const iconClass = computed(() => {
  const icons: Record<string, string> = {
    info: 'fa fa-info-circle',
    success: 'fa fa-check-circle',
    warning: 'fa fa-exclamation-triangle',
    error: 'fa fa-times-circle',
  }
  return icons[props.type]
})
</script>

<style scoped>
.fluent-message {
  display: flex; align-items: flex-start; gap: var(--fluent-spacing-sm);
  padding: var(--fluent-spacing-md) var(--fluent-spacing-lg);
  border-radius: var(--fluent-radius-md);
  font-size: var(--fluent-font-size-body);
  animation: fluent-fade-in var(--fluent-animation-duration) var(--fluent-animation-easing);
}
.fluent-message-info { background: var(--fluent-status-info-bg); color: var(--fluent-status-info); }
.fluent-message-success { background: var(--fluent-status-success-bg); color: var(--fluent-status-success); }
.fluent-message-warning { background: var(--fluent-status-warning-bg); color: var(--fluent-status-warning); }
.fluent-message-error { background: var(--fluent-status-error-bg); color: var(--fluent-status-error); }
.fluent-message-icon { font-size: 16px; margin-top: 1px; flex-shrink: 0; }
.fluent-message-content { flex: 1; }
</style>
