<template>
  <Teleport to="body">
    <Transition name="fluent-dialog">
      <div v-if="modelValue" class="fluent-dialog-overlay" @click.self="close">
        <div class="fluent-dialog" role="dialog" :aria-label="title">
          <div class="fluent-dialog-header">
            <h2 class="fluent-dialog-title">{{ title }}</h2>
            <button class="fluent-dialog-close" @click="close" aria-label="关闭">&times;</button>
          </div>
          <div class="fluent-dialog-body">
            <slot />
          </div>
          <div v-if="$slots.footer" class="fluent-dialog-footer">
            <slot name="footer" />
          </div>
        </div>
      </div>
    </Transition>
  </Teleport>
</template>

<script setup lang="ts">
const props = defineProps({
  modelValue: Boolean,
  title: { type: String, default: '' },
})
const emit = defineEmits<{ 'update:modelValue': [v: boolean] }>()
function close() { emit('update:modelValue', false) }
</script>

<style scoped>
.fluent-dialog-overlay {
  position: fixed; inset: 0;
  background: var(--fluent-bg-overlay);
  display: flex; align-items: center; justify-content: center;
  z-index: 9999;
  padding: var(--fluent-spacing-xl);
}
.fluent-dialog {
  background: var(--fluent-bg-card);
  border-radius: var(--fluent-radius-lg);
  box-shadow: var(--fluent-shadow-dialog);
  width: 100%;
  max-width: 480px;
  max-height: 90vh;
  display: flex;
  flex-direction: column;
  animation: fluent-scale-in var(--fluent-animation-duration) var(--fluent-animation-easing);
}
.fluent-dialog-header {
  display: flex; align-items: center; justify-content: space-between;
  padding: var(--fluent-spacing-xl) var(--fluent-spacing-xl) 0;
}
.fluent-dialog-title {
  font-size: var(--fluent-font-size-title);
  font-weight: 600; margin: 0; color: var(--fluent-text);
}
.fluent-dialog-close {
  background: none; border: none; font-size: 24px; color: var(--fluent-text-secondary);
  cursor: pointer; padding: 4px; line-height: 1; border-radius: var(--fluent-radius-sm);
  transition: color var(--fluent-animation-duration) var(--fluent-animation-easing);
}
.fluent-dialog-close:hover { color: var(--fluent-text); background: var(--fluent-bg-hover); }
.fluent-dialog-body { padding: var(--fluent-spacing-xl); overflow-y: auto; flex: 1; }
.fluent-dialog-footer {
  padding: var(--fluent-spacing-md) var(--fluent-spacing-xl);
  display: flex; gap: var(--fluent-spacing-sm); justify-content: flex-end;
  border-top: 1px solid var(--fluent-border);
}
.fluent-dialog-enter-active, .fluent-dialog-leave-active {
  transition: opacity var(--fluent-animation-duration) var(--fluent-animation-easing);
}
.fluent-dialog-enter-from, .fluent-dialog-leave-to { opacity: 0; }
</style>
