<template>
  <div class="vip-page container py-5">
    <div class="text-center mb-5">
      <h1 class="vip-title"><i class="fa fa-diamond text-warning me-2"></i>Cocokalo 大会员</h1>
      <p class="text-muted">开通大会员，畅享独家内容与特权</p>
    </div>

    <div class="row mb-5">
      <div v-for="plan in plans" :key="plan.name" class="col-md-4 mb-4">
        <div class="card vip-card text-center h-100" :class="{ popular: plan.popular }">
          <div v-if="plan.popular" class="card-header bg-warning text-white">推荐</div>
          <div class="card-body">
            <h3 class="card-title">{{ plan.name }}</h3>
            <p class="vip-price">{{ plan.price }}<small class="text-muted">/月</small></p>
            <ul class="list-unstyled text-start mb-4">
              <li v-for="feature in plan.features" :key="feature" class="mb-2">
                <i class="fa fa-check text-success me-2"></i>{{ feature }}
              </li>
            </ul>
            <button class="btn w-100" :class="plan.popular ? 'btn-warning' : 'btn-outline-warning'">立即开通</button>
          </div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-header"><i class="fa fa-star me-2"></i>大会员特权</div>
      <div class="card-body">
        <div class="row">
          <div v-for="privilege in privileges" :key="privilege.title" class="col-md-3 col-6 mb-4 text-center">
            <i :class="'fa ' + privilege.icon + ' fa-2x text-warning mb-2'"></i>
            <h6>{{ privilege.title }}</h6>
            <p class="small text-muted mb-0">{{ privilege.desc }}</p>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
const plans = [
  { name: '月度会员', price: '¥15', popular: false, features: ['去广告', '高清画质', '每月5次投币', '专属标识'] },
  { name: '年度会员', price: '¥128', popular: true, features: ['去广告', '高清画质', '无限投币', '专属标识', '月度礼包', '抢先看'] },
  { name: '大会员', price: '¥198', popular: false, features: ['去广告', '4K画质', '无限投币', '专属标识', '月度礼包', '抢先看', '独家内容', '客服优先'] },
]
const privileges = [
  { title: '去广告', icon: 'fa-ban', desc: '免除所有广告打扰' },
  { title: '高清画质', icon: 'fa-film', desc: '1080P/4K画质' },
  { title: '抢先看', icon: 'fa-fast-forward', desc: '新番抢先观看' },
  { title: '专属标识', icon: 'fa-id-badge', desc: '大会员身份标识' },
  { title: '月度礼包', icon: 'fa-gift', desc: '每月领取B币礼包' },
  { title: '独家内容', icon: 'fa-lock', desc: '会员专属番剧' },
  { title: '无限投币', icon: 'fa-coins', desc: '每日投币无限制' },
  { title: '客服优先', icon: 'fa-headset', desc: '专属客服通道' },
]
</script>

<style scoped>
.vip-title { font-size: 2.5rem; }
.vip-card { border: 2px solid #eee; transition: transform 0.2s; }
.vip-card:hover { transform: translateY(-5px); }
.vip-card.popular { border-color: #ffc107; box-shadow: 0 5px 20px rgba(255,193,7,0.3); }
.vip-price { font-size: 2rem; font-weight: 700; color: #ff6a00; }
</style>
