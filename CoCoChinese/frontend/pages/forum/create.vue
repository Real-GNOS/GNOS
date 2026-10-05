<template>
  <div class="forum-page">
    <div class="forum-container">
      <div class="breadcrumb-bar">
        <NuxtLink to="/forum">论坛</NuxtLink>
        <i class="fa fa-chevron-right sep"></i>
        <span>发布话题</span>
      </div>

      <div class="create-form">
        <h2>发布新话题</h2>

        <div class="form-group">
          <label>选择分类</label>
          <select v-model="categoryId">
            <option value="">请选择分类</option>
            <option v-for="cat in categories" :key="cat.id" :value="cat.id">{{ cat.name }}</option>
          </select>
        </div>

        <div class="form-group">
          <label>标题</label>
          <input v-model="title" type="text" placeholder="输入话题标题" maxlength="100">
        </div>

        <div class="form-group">
          <label>内容</label>
          <textarea v-model="content" placeholder="写下你想讨论的内容..." rows="8" maxlength="10000"></textarea>
          <div class="textarea-footer">
            <span class="char-count">{{ content.length }} / 10000</span>
          </div>
        </div>

        <div class="form-actions">
          <NuxtLink to="/forum" class="btn-cancel">取消</NuxtLink>
          <button class="btn-submit" @click="submitTopic" :disabled="!canSubmit || submitting || isBanned">
            {{ submitting ? '发布中...' : '发布话题' }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
const { user: me } = useUser()

if (!me.value) {
  await navigateTo('/login?redirect=/forum/create')
}

const { data: catData } = await useFetch('/api/forum/categories')
const categories = ref(catData.value?.data || [])

const categoryId = ref('')
const title = ref('')
const content = ref('')
const submitting = ref(false)

const canSubmit = computed(() => categoryId.value && title.value.trim() && content.value.trim())

async function submitTopic() {
  if (!canSubmit.value || submitting.value) return
  submitting.value = true
  try {
    const res = await $fetch('/api/forum/topics', {
      method: 'POST',
      body: {
        category_id: parseInt(categoryId.value),
        title: title.value.trim(),
        content: content.value.trim(),
      },
    })
    if (res.success && res.data) {
      navigateTo(`/forum/topic/${res.data.slug}`)
    }
  } catch (e) {
    alert(e.data?.message || '发布失败')
  }
  submitting.value = false
}
const { banned: isBanned } = useBan()
</script>

<style scoped>
.forum-page {
  min-height: calc(100vh - 60px);
  background: #f4f4f5;
  padding: 24px;
}
.forum-container {
  max-width: 860px;
  margin: 0 auto;
}
.breadcrumb-bar {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
  color: #999;
  margin-bottom: 16px;
}
.breadcrumb-bar a { color: #00a1d6; text-decoration: none; }
.breadcrumb-bar .sep { font-size: 10px; color: #ccc; }
.create-form {
  background: #fff;
  border-radius: 12px;
  padding: 28px;
  box-shadow: 0 1px 4px rgba(0,0,0,0.04);
}
.create-form h2 {
  font-size: 22px;
  font-weight: 700;
  margin: 0 0 24px;
  color: #222;
}
.form-group {
  margin-bottom: 20px;
}
.form-group label {
  display: block;
  font-size: 14px;
  font-weight: 600;
  color: #333;
  margin-bottom: 8px;
}
.form-group select, .form-group input, .form-group textarea {
  width: 100%;
  padding: 10px 14px;
  border: 1px solid #e8e8e8;
  border-radius: 8px;
  font-size: 14px;
  outline: none;
  transition: border-color 0.2s;
  box-sizing: border-box;
  font-family: inherit;
}
.form-group select:focus, .form-group input:focus, .form-group textarea:focus {
  border-color: #00a1d6;
}
.form-group textarea {
  resize: vertical;
  line-height: 1.6;
}
.textarea-footer {
  display: flex;
  justify-content: flex-end;
  margin-top: 4px;
}
.char-count { font-size: 12px; color: #bbb; }
.form-actions {
  display: flex;
  justify-content: flex-end;
  gap: 12px;
  margin-top: 24px;
  padding-top: 20px;
  border-top: 1px solid #f0f0f0;
}
.btn-cancel {
  padding: 10px 24px;
  border: 1px solid #ddd;
  border-radius: 8px;
  font-size: 14px;
  color: #666;
  text-decoration: none;
  transition: all 0.15s;
}
.btn-cancel:hover { border-color: #bbb; color: #333; }
.btn-submit {
  padding: 10px 24px;
  background: #00a1d6;
  color: #fff;
  border: none;
  border-radius: 8px;
  font-size: 14px;
  font-weight: 500;
  cursor: pointer;
  transition: background 0.2s;
}
.btn-submit:hover { background: #0088b3; }
.btn-submit:disabled { background: #ccc; cursor: not-allowed; }
@media (max-width: 767px) {
  .forum-page { padding: 16px; }
  .create-form { padding: 20px; }
}
</style>
