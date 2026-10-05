<template>
  <div class="subNavBox row text-center">
    <div class="col-2">
      <ul class="imgNavItemsBox">
        <li v-for="item in NavItems.imgNavItems" :key="item.text">
          <a :href="item.link">
            <b class="fa" :class="item.fontClass" aria-hidden="true"></b>
            <p>{{ item.text }}</p>
          </a>
        </li>
      </ul>
    </div>

    <div class="col-xl-8 col-7">
      <ul class="labelNavItemBox">
        <li v-for="(item, i) in visibleLabels" :key="i">
          <a :href="item.link">
            <i class="fa" :class="item.fontClass"></i>
            <span>{{ item.text }}</span>
            <small>{{ item.num }}</small>
          </a>
        </li>
        <li>
          <a :href="lastItem.link">
            <i class="fa" :class="lastItem.fontClass"></i>
            <span>{{ lastItem.text }}</span>
            <small>{{ lastItem.num }}</small>
          </a>
        </li>
      </ul>
    </div>

    <div class="col-xl-2 col-3">
      <ul class="addNavItemsBox">
        <li v-for="item in NavItems.addNavItems" :key="item.text">
          <a :href="item.link">
            <i class="fa" :class="item.fontClass" aria-hidden="true"></i>
            <p>{{ item.text }}</p>
          </a>
        </li>
      </ul>
    </div>
  </div>
</template>

<script setup>
const props = defineProps({ NavItems: { type: Object, default: () => ({ imgNavItems: [], labelNavItems: [], addNavItems: [] }) } })
const visibleLabels = computed(() => props.NavItems.labelNavItems.slice(0, 9))
const lastItem = computed(() => {
  const items = props.NavItems.labelNavItems
  return items[items.length - 1] || {}
})
</script>
