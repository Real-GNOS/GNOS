<template>
  <div class="fluent-field">
    <label v-if="label" :for="inputId" class="fluent-label">{{ label }}</label>
    <div class="fluent-field-content">
      <input
        :id="inputId"
        :type="type"
        :value="modelValue"
        :placeholder="placeholder"
        :disabled="disabled"
        :required="required"
        class="fluent-input"
        :class="{ 'fluent-input-error': error }"
        @input="$emit('update:modelValue', ($event.target as HTMLInputElement).value)"
      />
      <span v-if="error" class="fluent-field-error">{{ error }}</span>
    </div>
  </div>
</template>

<script setup lang="ts">
import { useId } from 'vue'
defineProps({
  modelValue: String,
  label: String,
  placeholder: String,
  type: { type: String, default: 'text' },
  disabled: Boolean,
  required: Boolean,
  error: String,
})
defineEmits<{ 'update:modelValue': [v: string] }>()
const inputId = `fluent-input-${useId()}`
</script>

<style scoped>
.fluent-field { margin-bottom: var(--fluent-spacing-lg); }
.fluent-field-content { position: relative; }
.fluent-input-error { border-color: var(--fluent-status-error) !important; }
.fluent-input-error:focus { box-shadow: 0 0 0 1px var(--fluent-status-error) !important; }
.fluent-field-error {
  font-size: var(--fluent-font-size-caption);
  color: var(--fluent-status-error);
  margin-top: var(--fluent-spacing-xs);
  display: block;
}
</style>
