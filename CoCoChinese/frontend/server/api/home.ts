const leftList = [
  { text: '文章', link: '/articles', sty: '' },
  { text: '图集', link: '/galleries', sty: '' },
  { text: '论坛', link: '/forum', sty: '' },
]
const rightList = [
  { text: '用户中心', link: '/user/center', sty: '' },
  { text: '大会员', link: '/vip', sty: '' },
  { text: '消息', link: '/messages', sty: '' },
  { text: '动态', link: '/dynamics', sty: '' },
  { text: '收藏', link: '/favorites', sty: '' },
  { text: '历史', link: '/history', sty: '' },
  { text: '创作中心', link: '/creator', sty: '' },
  { text: '投稿', link: '/upload', sty: '' },
]

const placard = ['公告栏内容(oﾟvﾟ)ノ']

const imgNavItems = [
  { num: '', link: '/', text: '动态', fontClass: 'fa-lightbulb-o' },
  { num: '', link: '/', text: '排行榜', fontClass: 'fa-bar-chart' },
]
const labelNavItems = [
  { num: '999', link: '/', text: '动画', fontClass: '' },
  { num: '999', link: '/', text: '音乐', fontClass: '' },
  { num: '999', link: '/', text: '舞蹈', fontClass: '' },
  { num: '999', link: '/', text: '知识', fontClass: '' },
  { num: '999', link: '/', text: '生活', fontClass: '' },
  { num: '233', link: '/', text: '时尚', fontClass: '' },
  { num: '999', link: '/', text: '娱乐', fontClass: '' },
  { num: '999', link: '/', text: '电影', fontClass: '' },
  { num: '287', link: '/', text: '萌宠', fontClass: '' },
  { num: '75', link: '/', text: '游戏', fontClass: '' },
  { num: '999', link: '/', text: '记录', fontClass: '' },
  { num: '453', link: '/', text: '数码', fontClass: '' },
  { num: '132', link: '/', text: '科技', fontClass: '' },
  { num: '114', link: '/', text: '资讯', fontClass: '' },
  { num: '514', link: '/', text: '美食', fontClass: '' },
]
const addNavItems = [
  { num: '', link: '/forum', text: '论坛', fontClass: 'fa-comments' },
]

function toVideoItem(v: any) {
  return {
    img: v.imageUrl || v.image_url || '',
    link: `/player/${v.slug || v.id}`,
    title: v.title || '',
    author: v.author || '',
    videoType: v.videoType || v.video_type || '',
    videoTime: v.videoTime || v.video_time || '',
    likeVolue: v.likeVolue || v.like_volue || '0',
    authorImg: v.authorImg || v.author_img || '',
    recommend: v.recommend || '',
    watchVolue: v.watchVolue || v.watch_volue || '0',
    watchPeople: v.watchPeople || v.watch_people || '0',
    introduction: v.introduction || '',
    id: String(v.id),
  }
}

const about = [
  { text: '关于我们', link: '/help', sty: '' },
  { text: '联系我们', link: '/help', sty: '' },
  { text: '友情链接', link: '/', sty: '' },
  { text: '用户协议', link: '/help', sty: '' },
  { text: '隐私政策', link: '/help', sty: '' },
]
const send = [
  { text: '帮助中心', link: '/help', sty: '' },
  { text: '技术论坛', link: '/forum', sty: '' },
  { text: '活动中心', link: '/', sty: '' },
  { text: '广告合作', link: '/', sty: '' },
  { text: '用户反馈', link: '/', sty: '' },
]
const imgSend = [
  { num: '', link: '/', text: '客户端下载', fontClass: 'fa-download' },
  { num: '', link: '/', text: '新浪微博', fontClass: 'fa-weibo' },
  { num: '', link: '/', text: '官方微信', fontClass: 'fa-weixin' },
]

export default defineEventHandler(async (event) => {
  try {
    const [carousels, videos] = await Promise.all([
      prisma.carousel.findMany({ orderBy: { order: 'asc' }, take: 3 }),
      prisma.video.findMany({ orderBy: [{ order: 'asc' }, { createdAt: 'desc' }], take: 24 }),
    ])

    const allVids = videos
    const chunk = (arr: any[], size: number) =>
      Array.from({ length: Math.ceil(arr.length / size) }, (_, i) =>
        arr.slice(i * size, i * size + size).map(toVideoItem)
      )

    let loginFlag = false
    let userInfo = null
    try {
      const auth = getAuthFromEvent(event)
      if (auth) {
        userInfo = { userId: auth.userId, username: auth.username, role: auth.role }
        loginFlag = true
      }
    } catch {
    }

    const carouselList = carousels.map((c: any) => ({
      ...c,
      image_url: c.imageUrl,
      link: c.imageUrl || c.link,
      url: c.link || '/'
    }))

    return {
      carouselList,
      mainNav: { leftList, rightList, loginFlag, userInfo },
      placard,
      NavItems: { imgNavItems, labelNavItems, addNavItems },
      footer: { about, send, imgSend },
      videoObjectList: {
        specialObject: { videosLineList: chunk(allVids.slice(0, 8), 4), type: 'special' },
        defaultObject: { videosLineList: chunk(allVids.slice(8, 12), 4), type: 'default' },
        imageObject: { videosLineList: chunk(allVids.slice(12, 16), 4), type: 'image' },
      },
    }
  } catch (err) {
    console.error('API error:', err)
    throw createError({ statusCode: 500, message: '服务器错误' })
  }
})
