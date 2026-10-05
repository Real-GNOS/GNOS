<template>
  <div class="fluent-tabs">
    <div class="fluent-tab-bar" role="tablist">
      <button
        v-for="tab in tabs"
        :key="tab.value"
        :class="['fluent-tab', { active: modelValue === tab.value }]"
        @click="$emit('update:modelValue', tab.value)"
        role="tab"
      >
        <i v-if="tab.icon" :class="tab.icon"></i>
        {{ tab.label }}
      </button>
    </div>
    <div class="fluent-tab-content">
      <slot />
    </div>
  </div>
</template>

<script setup lang="ts">
defineProps({
  tabs: { type: Array as PropType<{ label: string; value: string; icon?: string }[]>, required: true },
  modelValue: { type: String, required: true },
})
defineEmits<{ 'update:modelValue': [v: string] }>()
</script>

<style scoped>
.fluent-tab-bar {
  display: flex; gap: 0; border-bottom: 1px solid var(--fluent-border);
  margin-bottom: var(--fluent-spacing-lg);
  overflow-x: auto;
}
.fluent-tab {
  display: flex; align-items: center; gap: var(--fluent-spacing-xs);
  padding: var(--fluent-spacing-sm) var(--fluent-spacing-lg);
  font-family: var(--fluent-font-sans);
  font-size: var(--fluent-font-size-body);
  font-weight: 500;
  color: var(--fluent-text-secondary);
  background: none;
  border: none;
  border-bottom: 2px solid transparent;
  cursor: pointer;
  transition: color var(--fluent-animation-duration) var(--fluent-animation-easing),
              border-color var(--fluent-animation-duration) var(--fluent-animation-easing);
  white-space: nowrap;
}
.fluent-tab:hover { color: var(--fluent-text); background: var(--fluent-bg-hover); }
.fluent-tab.active {
  color: var(--fluent-accent);
  border-bottom-color: var(--fluent-accent);
}
.fluent-tab-content { animation: fluent-fade-in var(--fluent-animation-duration) var(--fluent-animation-easing); }
</style>
