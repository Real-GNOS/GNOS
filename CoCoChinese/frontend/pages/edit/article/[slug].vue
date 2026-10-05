<template>
  <div class="article-editor container py-4">
    <h2 class="mb-4">编辑文章</h2>
    <div v-if="loading" class="text-center py-5"><i class="fa fa-spinner fa-spin fa-2x text-muted"></i></div>
    <div v-else-if="!article" class="text-center py-5">
      <p class="text-muted">文章不存在或无权编辑</p>
      <NuxtLink to="/articles" class="btn btn-primary">返回</NuxtLink>
    </div>
    <div v-else class="card">
      <div class="card-body">
        <form @submit.prevent="handleSave">
          <div class="mb-3">
            <label class="form-label">标题</label>
            <input v-model="title" type="text" class="form-control form-control-lg" maxlength="200" required>
          </div>
          <div class="row mb-3">
            <div class="col-md-6">
              <label class="form-label">分类</label>
              <select v-model="category" class="form-select">
                <option value="">未分类</option>
                <option value="科技">科技</option>
                <option value="生活">生活</option>
                <option value="游戏">游戏</option>
                <option value="教育">教育</option>
                <option value="文化">文化</option>
                <option value="其他">其他</option>
              </select>
            </div>
            <div class="col-md-6">
              <label class="form-label">标签</label>
              <input v-model="tagsStr" type="text" class="form-control" placeholder="用逗号分隔">
            </div>
          </div>
          <div class="mb-3">
            <label class="form-label">封面图</label>
            <div class="d-flex align-items-center gap-3">
              <img v-if="coverPreview" :src="coverPreview" alt="" style="width:120px;height:68px;object-fit:cover;border-radius:4px;">
              <input type="file" accept="image/*" class="form-control" @change="handleCoverChange">
              <small class="text-muted">不选择则保留原有封面</small>
            </div>
          </div>
          <div class="mb-3">
            <label class="form-label">正文</label>
            <div class="editor-toolbar" v-if="editor">
              <button type="button" :class="{ active: editor.isActive('bold') }" @click="editor.chain().focus().toggleBold().run()"><b>B</b></button>
              <button type="button" :class="{ active: editor.isActive('italic') }" @click="editor.chain().focus().toggleItalic().run()"><i>I</i></button>
              <button type="button" :class="{ active: editor.isActive('heading', { level: 2 }) }" @click="editor.chain().focus().toggleHeading({ level: 2 }).run()">H2</button>
              <button type="button" :class="{ active: editor.isActive('heading', { level: 3 }) }" @click="editor.chain().focus().toggleHeading({ level: 3 }).run()">H3</button>
              <button type="button" :class="{ active: editor.isActive('bulletList') }" @click="editor.chain().focus().toggleBulletList().run()">•</button>
              <button type="button" :class="{ active: editor.isActive('orderedList') }" @click="editor.chain().focus().toggleOrderedList().run()">1.</button>
              <button type="button" :class="{ active: editor.isActive('blockquote') }" @click="editor.chain().focus().toggleBlockquote().run()">"</button>
              <button type="button" @click="addImage">🖼</button>
              <button type="button" @click="addLink">🔗</button>
            </div>
            <editor-content :editor="editor" class="editor-content" />
          </div>
          <div class="mb-3">
            <label class="form-label">版本说明（可选，将记录到 git 提交信息）</label>
            <input v-model="versionMessage" type="text" class="form-control" placeholder="例如：修正错别字、更新封面图" maxlength="500">
          </div>
          <div class="d-flex gap-2 justify-content-end">
            <NuxtLink :to="'/articles/' + article.slug" class="btn btn-secondary">取消</NuxtLink>
            <button type="submit" class="btn btn-primary px-4" :disabled="saving || isBanned">
              {{ saving ? '保存中...' : '保存修改' }}
            </button>
          </div>
        </form>
      </div>
    </div>

    <!-- 版本历史：git 里的每次保存/发布快照 -->
    <div v-if="article" class="card mt-4">
      <div class="card-header d-flex justify-content-between align-items-center">
        <span>版本历史 <small class="text-muted">（基于 git，共 {{ versions.length }} 个版本）</small></span>
        <button class="btn btn-sm btn-outline-secondary" type="button" @click="loadVersions">
          <i class="fa fa-refresh" :class="{ 'fa-spin': loadingVersions }"></i> 刷新
        </button>
      </div>
      <div class="card-body">
        <div v-if="loadingVersions" class="text-center py-3"><i class="fa fa-spinner fa-spin text-muted"></i></div>
        <div v-else-if="!versions.length" class="text-center py-3 text-muted">暂无版本记录</div>
        <ul v-else class="list-group">
          <li v-for="(v, i) in versions" :key="v.sha" class="list-group-item d-flex flex-wrap align-items-center gap-2">
            <span class="badge" :class="i === 0 ? 'bg-primary' : 'bg-secondary'">v{{ versions.length - i }}</span>
            <span v-if="v.tag" class="badge bg-warning text-dark">{{ v.tag }}</span>
            <div class="flex-grow-1">
              <div class="fw-semibold">{{ v.message }}</div>
              <small class="text-muted">{{ formatTime(v.date) }} · {{ v.operator_name || '系统' }}</small>
            </div>
            <small class="text-monospace text-muted">{{ v.sha.slice(0, 8) }}</small>
            <div class="btn-group btn-group-sm">
              <button class="btn btn-outline-primary" type="button" @click="viewDiff(v.sha)">对比</button>
              <button
                v-if="i > 0"
                class="btn btn-outline-danger"
                type="button"
                :disabled="rollingBack === v.sha"
                @click="rollbackTo(v.sha)"
              >
                {{ rollingBack === v.sha ? '回滚中...' : '回滚到此版本' }}
              </button>
              <span v-else class="btn btn-outline-secondary disabled">当前版本</span>
            </div>
          </li>
        </ul>
      </div>
    </div>

    <!-- diff 弹窗 -->
    <div v-if="showDiff" class="modal fade show d-block" tabindex="-1" @click.self="showDiff = false">
      <div class="modal-dialog modal-lg modal-dialog-scrollable">
        <div class="modal-content">
          <div class="modal-header">
            <h5 class="modal-title">版本差异</h5>
            <button type="button" class="btn-close" @click="showDiff = false"></button>
          </div>
          <div class="modal-body">
            <p class="text-muted small">{{ diffSummary }}</p>
            <pre class="diff-view">{{ diffText }}</pre>
          </div>
          <div class="modal-footer">
            <button type="button" class="btn btn-secondary" @click="showDiff = false">关闭</button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { useEditor, EditorContent } from '@tiptap/vue-3'
import StarterKit from '@tiptap/starter-kit'
import Image from '@tiptap/extension-image'
import Link from '@tiptap/extension-link'
import Placeholder from '@tiptap/extension-placeholder'

const route = useRoute()
const article = ref(null)
const loading = ref(true)
const title = ref('')
const category = ref('')
const tagsStr = ref('')
const coverFile = ref(null)
const coverPreview = ref('')
const saving = ref(false)
const versionMessage = ref('')
const versions = ref([])
const loadingVersions = ref(false)
const rollingBack = ref('')
const showDiff = ref(false)
const diffText = ref('')
const diffSummary = ref('')

function formatTime(ts) {
  if (!ts) return '-'
  const d = new Date(ts)
  if (Number.isNaN(d.getTime())) return ts
  const pad = (n) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`
}

async function loadVersions() {
  loadingVersions.value = true
  try {
    const res = await $fetch(`/api/articles/${route.params.slug}/versions`)
    versions.value = res.versions || []
  } catch {} finally {
    loadingVersions.value = false
  }
}

async function viewDiff(sha) {
  if (!versions.value.length) return
  const current = versions.value[0].sha
  if (current === sha) {
    alert('当前版本没有差异可对比')
    return
  }
  try {
    const res = await $fetch(`/api/articles/${route.params.slug}/versions/diff?from=${sha}&to=${current}`)
    diffSummary.value = res.summary || ''
    diffText.value = res.diff || '(无文本差异)'
    showDiff.value = true
  } catch {
    alert('对比失败')
  }
}

async function rollbackTo(sha) {
  if (!confirm('确定回滚到此版本？当前内容将被覆盖（可在版本历史中再次回滚恢复）。')) return
  rollingBack.value = sha
  try {
    await $fetch(`/api/articles/${route.params.slug}/versions/rollback`, {
      method: 'POST',
      body: { sha },
    })
    alert('回滚成功，正在重新加载...')
    await fetchArticle()
    await loadVersions()
  } catch (e) {
    alert(`回滚失败：${e?.data?.message || '未知错误'}`)
  } finally {
    rollingBack.value = ''
  }
}

const editor = useEditor({
  content: '',
  extensions: [
    StarterKit,
    Image,
    Link.configure({ openOnClick: false }),
    Placeholder.configure({ placeholder: '开始写文章...' }),
  ],
})

function handleCoverChange(e) {
  const file = e.target.files?.[0]
  if (file) {
    coverFile.value = file
    coverPreview.value = URL.createObjectURL(file)
  }
}

async function addImage() {
  const input = document.createElement('input')
  input.type = 'file'
  input.accept = 'image/jpeg,image/png,image/gif,image/webp'
  input.onchange = async () => {
    const file = input.files?.[0]
    if (!file) return
    const fd = new FormData()
    fd.append('file', file)
    try {
      const res = await $fetch('/api/upload/article-image', { method: 'POST', body: fd })
      if (res.success) {
        editor.value?.chain().focus().setImage({ src: res.url }).run()
      }
    } catch {
      alert('图片上传失败')
    }
  }
  input.click()
}

function addLink() {
  const url = prompt('输入链接地址:')
  if (url) {
    editor.value?.chain().focus().setLink({ href: url }).run()
  }
}

async function fetchArticle() {
  try {
    const res = await $fetch(`/api/articles/${route.params.slug}`)
    if (res.success) {
      article.value = res.data
      title.value = res.data.title
      category.value = res.data.category || ''
      tagsStr.value = (res.data.tags || []).join(', ')
      coverPreview.value = res.data.cover_image || ''
      if (editor.value) {
        editor.value.commands.setContent(res.data.content || '')
      }
    }
  } catch {} finally {
    loading.value = false
  }
}

async function handleSave() {
  if (!title.value.trim()) return
  saving.value = true
  try {
    let coverImage = coverPreview.value
    if (coverFile.value) {
      const fd = new FormData()
      fd.append('file', coverFile.value)
      const res = await $fetch('/api/upload/article-image', { method: 'POST', body: fd })
      if (res.success) coverImage = res.url
    }
    const tags = tagsStr.value ? tagsStr.value.split(',').map(t => t.trim()).filter(Boolean) : []
    await $fetch(`/api/articles/${route.params.slug}`, {
      method: 'PUT',
      body: {
        title: title.value.trim(),
        content: editor.value?.getHTML() || '',
        category: category.value,
        tags,
        cover_image: coverImage,
        message: versionMessage.value || '',
      },
    })
    navigateTo('/articles/' + route.params.slug)
  } catch {
    alert('保存失败')
  } finally {
    saving.value = false
  }
}

onMounted(() => {
  fetchArticle()
  loadVersions()
})
const { banned: isBanned } = useBan()
</script>

<style scoped>
.article-editor { max-width: 960px; margin: 0 auto; }
.editor-toolbar {
  display: flex; gap: 4px; padding: 8px; border: 1px solid #ddd; border-bottom: none;
  border-radius: 8px 8px 0 0; background: #f8f9fa; flex-wrap: wrap;
}
.editor-toolbar button {
  background: none; border: 1px solid transparent; padding: 4px 10px;
  border-radius: 4px; cursor: pointer; font-size: 14px; color: #555; transition: all 0.15s;
}
.editor-toolbar button:hover { background: #e9ecef; }
.editor-toolbar button.active { background: #e8f4fe; color: #00a1d6; border-color: #b8daff; }
.editor-content {
  border: 1px solid #ddd; border-radius: 0 0 8px 8px; padding: 16px;
  min-height: 400px; cursor: text;
}
.editor-content :deep(.ProseMirror) { outline: none; min-height: 400px; }
.editor-content :deep(.ProseMirror p) { margin: 0 0 8px; line-height: 1.8; }
.editor-content :deep(.ProseMirror h2) { margin: 24px 0 12px; font-size: 22px; }
.editor-content :deep(.ProseMirror h3) { margin: 20px 0 10px; font-size: 18px; }
.editor-content :deep(.ProseMirror img) { max-width: 100%; border-radius: 8px; margin: 12px 0; }
.editor-content :deep(.ProseMirror blockquote) {
  border-left: 4px solid #00a1d6; padding: 8px 16px; margin: 12px 0;
  background: #f8f9fa; border-radius: 0 8px 8px 0; color: #666;
}
.editor-content :deep(.ProseMirror ul), .editor-content :deep(.ProseMirror ol) { padding-left: 24px; }
.editor-content :deep(.ProseMirror a) { color: #00a1d6; text-decoration: underline; }
.editor-content :deep(.ProseMirror p.is-editor-empty:first-child::before) {
  content: attr(data-placeholder); color: #bbb; pointer-events: none; float: left; height: 0;
}
.diff-view {
  background: #f8f9fa; border: 1px solid #dee2e6; border-radius: 6px;
  padding: 12px; font-size: 12px; line-height: 1.6; max-height: 420px;
  overflow: auto; white-space: pre-wrap; word-break: break-all;
}
.modal-backdrop { z-index: 1040; }
.modal.show { z-index: 1050; }
</style>
