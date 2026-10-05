<template>
  <div class="article-editor container py-4">
    <h2 class="mb-4">写文章</h2>
    <div class="card">
      <div class="card-body">
        <form @submit.prevent="handleSave">
          <div class="mb-3">
            <label class="form-label">标题</label>
            <input v-model="title" type="text" class="form-control form-control-lg" maxlength="200" required placeholder="输入文章标题">
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
            </div>
          </div>

          <div class="mb-3">
            <label class="form-label">正文</label>
            <div class="editor-toolbar" v-if="editor">
              <button type="button" :class="{ active: editor.isActive('bold') }" @click="editor.chain().focus().toggleBold().run()" title="加粗"><b>B</b></button>
              <button type="button" :class="{ active: editor.isActive('italic') }" @click="editor.chain().focus().toggleItalic().run()" title="斜体"><i>I</i></button>
              <button type="button" :class="{ active: editor.isActive('heading', { level: 2 }) }" @click="editor.chain().focus().toggleHeading({ level: 2 }).run()" title="标题">H2</button>
              <button type="button" :class="{ active: editor.isActive('heading', { level: 3 }) }" @click="editor.chain().focus().toggleHeading({ level: 3 }).run()" title="小标题">H3</button>
              <button type="button" :class="{ active: editor.isActive('bulletList') }" @click="editor.chain().focus().toggleBulletList().run()" title="列表">•</button>
              <button type="button" :class="{ active: editor.isActive('orderedList') }" @click="editor.chain().focus().toggleOrderedList().run()" title="编号列表">1.</button>
              <button type="button" :class="{ active: editor.isActive('blockquote') }" @click="editor.chain().focus().toggleBlockquote().run()" title="引用">"</button>
              <button type="button" @click="addImage" title="插入图片">🖼</button>
              <button type="button" @click="addLink" title="插入链接">🔗</button>
            </div>
            <editor-content :editor="editor" class="editor-content" />
          </div>

          <div class="d-flex gap-2 justify-content-end">
            <NuxtLink to="/articles" class="btn btn-secondary">取消</NuxtLink>
            <button type="submit" class="btn btn-primary px-4" :disabled="saving || isBanned">
              {{ saving ? '发布中...' : '发布文章' }}
            </button>
          </div>
        </form>
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

const title = ref('')
const category = ref('')
const tagsStr = ref('')
const coverFile = ref(null)
const coverPreview = ref('')
const saving = ref(false)

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

async function handleSave() {
  if (!title.value.trim() || !editor.value?.getHTML().trim()) return
  saving.value = true
  try {
    let coverImage = ''
    if (coverFile.value) {
      const fd = new FormData()
      fd.append('file', coverFile.value)
      const res = await $fetch('/api/upload/article-image', { method: 'POST', body: fd })
      if (res.success) coverImage = res.url
    }
    const tags = tagsStr.value ? tagsStr.value.split(',').map(t => t.trim()).filter(Boolean) : []
    await $fetch('/api/articles', {
      method: 'POST',
      body: {
        title: title.value.trim(),
        content: editor.value?.getHTML() || '',
        category: category.value,
        tags,
        cover_image: coverImage,
      },
    })
    navigateTo('/articles')
  } catch {
    alert('发布失败')
  } finally {
    saving.value = false
  }
}
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
</style>
