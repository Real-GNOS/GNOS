<template>
  <component
    :is="tag"
    :class="['fluent-btn', `fluent-btn-${variant}`, { 'fluent-btn-block': block, 'fluent-btn-loading': loading }]"
    :disabled="disabled || loading"
    v-bind="$attrs"
    @click="$emit('click', $event)"
  >
    <span v-if="loading" class="fluent-btn-spinner"></span>
    <i v-if="icon && !loading" :class="icon"></i>
    <slot />
  </component>
</template>

<script setup lang="ts">
defineProps({
  variant: { type: String, default: 'primary', validator: v => ['primary', 'secondary', 'danger', 'subtle'].includes(v) },
  block: Boolean,
  loading: Boolean,
  disabled: Boolean,
  icon: String,
  tag: { type: String, default: 'button' },
})
defineEmits<{ click: [e: MouseEvent] }>()
</script>

<style scoped>
.fluent-btn { gap: 6px; position: relative; overflow: hidden; }
.fluent-btn-block { width: 100%; }
.fluent-btn:disabled { opacity: 0.4; cursor: not-allowed; pointer-events: none; }
.fluent-btn-subtle {
  background: transparent; color: var(--fluent-text); border-color: transparent;
}
.fluent-btn-subtle:hover { background: var(--fluent-bg-hover); }
.fluent-btn-subtle:active { background: var(--fluent-bg-active); }
.fluent-btn-loading { position: relative; }
.fluent-btn-spinner {
  width: 14px; height: 14px;
  border: 2px solid currentColor;
  border-right-color: transparent;
  border-radius: 50%;
  animation: fluent-spin 0.8s linear infinite;
}
</style>
