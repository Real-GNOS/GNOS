<template>
  <Teleport to="body">
    <TransitionGroup name="fluent-toast" tag="div" class="fluent-toast-container">
      <div v-for="toast in toasts" :key="toast.id"
        :class="['fluent-toast', `fluent-toast-${toast.type}`]"
        @click="remove(toast.id)">
        <i :class="toast.icon" class="fluent-toast-icon"></i>
        <span class="fluent-toast-message">{{ toast.message }}</span>
      </div>
    </TransitionGroup>
  </Teleport>
</template>

<script setup lang="ts">
interface Toast {
  id: number
  type: 'success' | 'error' | 'info' | 'warning'
  message: string
  icon: string
}

const toasts = ref<Toast[]>([])
let nextId = 1

function add(type: Toast['type'], message: string, duration = 4000) {
  const icons = { success: 'fa fa-check-circle', error: 'fa fa-times-circle', info: 'fa fa-info-circle', warning: 'fa fa-exclamation-circle' }
  const toast: Toast = { id: nextId++, type, message, icon: icons[type] }
  toasts.value.push(toast)
  setTimeout(() => remove(toast.id), duration)
}

function remove(id: number) {
  const idx = toasts.value.findIndex(t => t.id === id)
  if (idx >= 0) toasts.value.splice(idx, 1)
}

function success(msg: string) { add('success', msg) }
function error(msg: string) { add('error', msg) }
function info(msg: string) { add('info', msg) }
function warning(msg: string) { add('warning', msg) }

defineExpose({ success, error, info, warning })
</script>

<style scoped>
.fluent-toast-container {
  position: fixed; top: var(--fluent-spacing-xl); right: var(--fluent-spacing-xl);
  z-index: 10000; display: flex; flex-direction: column; gap: var(--fluent-spacing-sm);
  pointer-events: none;
}
.fluent-toast {
  display: flex; align-items: center; gap: var(--fluent-spacing-sm);
  padding: var(--fluent-spacing-md) var(--fluent-spacing-lg);
  border-radius: var(--fluent-radius-md);
  box-shadow: var(--fluent-shadow-flyout);
  cursor: pointer;
  pointer-events: auto;
  max-width: 360px;
  animation: fluent-slide-up var(--fluent-animation-duration) var(--fluent-animation-easing);
}
.fluent-toast-success { background: #dff6dd; color: #107c10; }
.fluent-toast-error { background: #fde7e9; color: #d13438; }
.fluent-toast-info { background: #deecf9; color: #0078d4; }
.fluent-toast-warning { background: #fff4ce; color: #ff8c00; }
.fluent-toast-icon { font-size: 16px; }
.fluent-toast-message { font-size: var(--fluent-font-size-body); font-weight: 500; }
.fluent-toast-enter-active, .fluent-toast-leave-active { transition: all 200ms var(--fluent-animation-easing); }
.fluent-toast-enter-from { opacity: 0; transform: translateX(100%); }
.fluent-toast-leave-to { opacity: 0; transform: translateX(100%); }
</style>
