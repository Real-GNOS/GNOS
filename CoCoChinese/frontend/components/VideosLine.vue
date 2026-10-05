<template>
  <ul class="videosLineBox">
    <li v-for="(video, i) in videosLine" :key="i">
      <div class="videoBar">
        <div class="videoBox">
          <a :href="video.link">
            <img :alt="video.title" :src="video.img || '/images/videoImg.webp'">
            <div v-if="type === 'special'" class="specialVideoModel">
              <h5>{{ video.title }}</h5>
              <p><i class="fa fa-user-circle-o" aria-hidden="true"></i> {{ video.author }}</p>
              <p>{{ video.watchVolue }}播放</p>
            </div>
            <div v-else-if="type === 'default'" class="defaultVideoModel">
              <span><i class="fa fa-eye" aria-hidden="true"></i> {{ video.watchVolue }}</span>
              <span><i class="fa fa-thumbs-o-up" aria-hidden="true"></i> {{ video.likeVolue }}</span>
              <span><i class="fa fa-clock-o" aria-hidden="true"></i> {{ video.videoTime }}</span>
            </div>
            <div v-else-if="type === 'image'" class="imageVideoModel">
              <span><i class="fa fa-users" aria-hidden="true"></i> {{ video.watchPeople }}</span>
            </div>
          </a>
          <div v-if="type !== 'image'" class="seeLaterModel clearfix">
            <i class="fa fa-clock-o" aria-hidden="true"></i>
            <p>稍后再康</p>
          </div>
          <a v-if="type !== 'special'" :href="video.link" class="seeNowModel clearfix">
            <i class="fa fa-eye" aria-hidden="true"></i>
            <span>让我康康</span>
          </a>
        </div>
      </div>
      <template v-if="type !== 'special'">
        <a :href="video.link" class="videoDataBar">
          <template v-if="type === 'default'">
            <div class="defaultObjectData">
              <p>{{ video.recommend ? video.recommend + video.title : video.title }}</p>
            </div>
          </template>
          <template v-else-if="type === 'image'">
            <div class="imageObjectData">
              <img class="col-xl-2 col-3" :alt="video.author" :src="video.authorImg || '/images/default_avatar.png'">
              <div class="col-xl-10 col-9">
                <h6>{{ video.author }}</h6>
                <p>{{ video.introduction }}</p>
                <small>{{ video.videoType }}</small>
              </div>
            </div>
          </template>
        </a>
      </template>
    </li>
  </ul>
</template>

<script setup>
defineProps({
  videosLine: { type: Array, default: () => [] },
  type: { type: String, default: 'default' }
})
</script>

<style scoped>
.videosLineBox { list-style: none; padding: 0; margin: 0; display: flex; flex-wrap: wrap; gap: 15px; }
.videosLineBox li { flex: 1 1 calc(25% - 15px); min-width: 200px; }
.videoBar { position: relative; }
.videoBox { position: relative; overflow: hidden; border-radius: 8px; }
.videoBox a img { width: 100%; display: block; border-radius: 8px; }
.specialVideoModel { position: absolute; bottom: 0; left: 0; right: 0; padding: 10px; background: linear-gradient(to top, rgba(0,0,0,0.8), transparent); color: #fff; }
.specialVideoModel h5 { margin: 0 0 5px; font-size: 14px; }
.specialVideoModel p { margin: 0; font-size: 12px; }
.defaultVideoModel { position: absolute; bottom: 0; left: 0; right: 0; display: flex; justify-content: space-between; padding: 5px 8px; background: rgba(0,0,0,0.6); color: #fff; font-size: 11px; }
.imageVideoModel { position: absolute; top: 5px; right: 5px; background: rgba(0,0,0,0.6); color: #fff; padding: 2px 6px; border-radius: 4px; font-size: 11px; }
.seeLaterModel { position: absolute; top: 5px; right: 5px; background: rgba(0,0,0,0.6); color: #fff; padding: 2px 6px; border-radius: 4px; font-size: 11px; cursor: pointer; display: none; }
.videoBox:hover .seeLaterModel { display: block; }
.seeNowModel { position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); background: rgba(0,0,0,0.7); color: #fff; padding: 8px 15px; border-radius: 20px; text-decoration: none; font-size: 13px; display: none; }
.videoBox:hover .seeNowModel { display: flex; align-items: center; gap: 5px; }
.videoDataBar { text-decoration: none; color: #333; display: block; margin-top: 8px; }
.defaultObjectData p { margin: 0; font-size: 13px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.imageObjectData { display: flex; align-items: center; gap: 10px; }
.imageObjectData img { border-radius: 50%; width: 40px; height: 40px; }
.imageObjectData h6 { margin: 0; font-size: 13px; }
.imageObjectData p { margin: 0; font-size: 12px; color: #666; }
.imageObjectData small { font-size: 11px; color: #999; }
</style>
