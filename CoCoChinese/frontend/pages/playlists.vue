<template>
  <div class="playlists-page">
    <div class="playlists-header">
      <h1><i class="fa fa-list"></i> 播放列表</h1>
      <FluentButton variant="primary" icon="fa fa-plus" @click="showCreate = true">新建播放列表</FluentButton>
    </div>

    <FluentDialog v-model="showCreate" title="新建播放列表">
      <FluentTextField v-model="newName" label="名称" placeholder="输入播放列表名称" />
      <FluentTextField v-model="newDesc" label="描述" placeholder="选填" />
      <label class="fluent-checkbox">
        <input v-model="newPublic" type="checkbox" />
        <span>公开</span>
      </label>
      <template #footer>
        <FluentButton variant="secondary" @click="showCreate = false">取消</FluentButton>
        <FluentButton variant="primary" @click="createPlaylist">创建</FluentButton>
      </template>
    </FluentDialog>

    <div v-if="playlists.length" class="playlists-grid">
      <div v-for="pl in playlists" :key="pl.id" class="playlist-card fluent-card">
        <div class="playlist-card-body">
          <h3 class="playlist-name">{{ pl.name }}</h3>
          <p v-if="pl.description" class="playlist-desc">{{ pl.description }}</p>
          <div class="playlist-meta">
            <span><i class="fa fa-film"></i> {{ pl.video_count || 0 }} 个视频</span>
            <span v-if="pl.is_public"><i class="fa fa-globe"></i> 公开</span>
          </div>
        </div>
        <div class="playlist-card-footer">
          <NuxtLink :to="`/playlists/${pl.id}`" class="fluent-btn fluent-btn-primary">
            查看
          </NuxtLink>
          <FluentButton variant="secondary" icon="fa fa-trash" @click="deletePlaylist(pl.id)">删除</FluentButton>
        </div>
      </div>
    </div>
    <div v-else class="empty-state">
      <i class="fa fa-list fa-4x"></i>
      <p>还没有播放列表</p>
      <FluentButton variant="primary" @click="showCreate = true">创建第一个</FluentButton>
    </div>
  </div>
</template>

<script setup lang="ts">
const { success, error } = useToast()
const playlists = ref<any[]>([])
const showCreate = ref(false)
const newName = ref('')
const newDesc = ref('')
const newPublic = ref(true)

onMounted(async () => {
  try {
    const res: any = await $fetch('/api/playlists')
    playlists.value = res.playlists
  } catch {}
})

async function createPlaylist() {
  if (!newName.value) return
  try {
    const res: any = await $fetch('/api/playlists', {
      method: 'POST',
      body: { name: newName.value, description: newDesc.value, isPublic: newPublic.value },
    })
    playlists.value.unshift(res.playlist)
    showCreate.value = false
    newName.value = ''
    newDesc.value = ''
    success('播放列表已创建')
  } catch (e: any) {
    error(e?.data?.message || '创建失败')
  }
}

async function deletePlaylist(id: number) {
  if (!confirm('确定删除？')) return
  try {
    await $fetch(`/api/playlists/${id}`, { method: 'DELETE' })
    playlists.value = playlists.value.filter(p => p.id !== id)
    success('已删除')
  } catch {
    error('删除失败')
  }
}
</script>

<style scoped>
.playlists-page { max-width: 1000px; margin: 0 auto; padding: var(--fluent-spacing-2xl) var(--fluent-spacing-xl); }
.playlists-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: var(--fluent-spacing-2xl); }
.playlists-header h1 { font-size: var(--fluent-font-size-title-large); margin: 0; }
.playlists-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: var(--fluent-spacing-lg); }
.playlist-card { display: flex; flex-direction: column; }
.playlist-card-body { flex: 1; padding: var(--fluent-spacing-xl); }
.playlist-name { font-size: var(--fluent-font-size-title); margin: 0 0 var(--fluent-spacing-xs); }
.playlist-desc { color: var(--fluent-text-secondary); font-size: var(--fluent-font-size-body); margin: 0 0 var(--fluent-spacing-md); }
.playlist-meta { display: flex; gap: var(--fluent-spacing-lg); font-size: var(--fluent-font-size-caption); color: var(--fluent-text-secondary); }
.playlist-meta i { margin-right: 4px; }
.playlist-card-footer { display: flex; gap: var(--fluent-spacing-sm); padding: var(--fluent-spacing-md) var(--fluent-spacing-xl); border-top: 1px solid var(--fluent-border); }
.empty-state { text-align: center; padding: var(--fluent-spacing-4xl) 0; color: var(--fluent-text-secondary); }
.empty-state i { margin-bottom: var(--fluent-spacing-lg); }
.empty-state p { margin-bottom: var(--fluent-spacing-lg); }
.fluent-checkbox { display: flex; align-items: center; gap: var(--fluent-spacing-sm); font-size: var(--fluent-font-size-body); cursor: pointer; }
</style>
